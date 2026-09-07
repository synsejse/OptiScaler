"""Narrow, lossless whole-subresource R32 typeless-depth copy admission."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'


class FogDepthCopy(unittest.TestCase):
    def test_host_keeps_original_whole_copy_and_native_state_protocol(self):
        source = (BASE / 'FSRDCyberpunkFogProbe.cpp').read_text()
        prepare = source.split('void PrepareFogDepth(', 1)[1].split('\nvoid RecordFogDepth(', 1)[0]
        record = source.split('void RecordFogDepth(', 1)[1].split('\nstd::shared_ptr<CapturePlan> PrepareCapture(', 1)[0]
        self.assertIn('CyberpunkFogDepthCopy::ClassifySource(desc, d.srvFormat,', prepare)
        self.assertIn('copyKind == FSRD::CyberpunkFogDepthCopy::SourceKind::Refused', prepare)
        self.assertIn('desc.Width != color.Width || desc.Height != color.Height', prepare)
        self.assertIn('CyberpunkFogDepthCopy::AdmitDestination(output, dimensions[0], dimensions[1])', prepare)
        self.assertLess(prepare.index('CyberpunkFogDepthCopy::ClassifySource('),
                        prepare.index('CreateCommittedResource('))
        self.assertLess(prepare.index('CyberpunkFogDepthCopy::AdmitDestination('),
                        prepare.index('CreateCommittedResource('))
        self.assertIn('sourceId.Get() == mainId.Get()', prepare)
        self.assertIn('sourceDeviceId.Get() != targetDeviceId.Get()', prepare)
        self.assertIn('plan.retainedTextureBytes += sourceBytes + outputBytes', prepare)
        self.assertIn('plan.depthSource = {}', prepare)
        self.assertIn('plan.depthOutput.Reset()', prepare)
        self.assertIn('input.copySource = true', record)
        self.assertIn('CyberpunkFogDenoiseAccess::RecordPrivateCompute(host, input,', record)
        self.assertIn('source(plan.depthSource.resource.Get(), 0)', record)
        self.assertIn('target(plan.depthOutput.Get(), 0)', record)
        self.assertEqual(record.count('CopyTextureRegion('), 1)
        self.assertIn('CopyTextureRegion(&target, 0, 0, 0, &source, nullptr)', record)
        self.assertIn('Transition(list, plan.depthOutput.Get(), D3D12_RESOURCE_STATE_COPY_DEST,', record)
        self.assertNotIn('Transition(list, plan.depthSource', record)
        self.assertNotIn('ResourceBarrier(', record)
        self.assertNotIn('Dispatch(', record)
        self.assertLess(record.index('Outcome::PrivateRecordedRestored'),
                        record.index('plan.layers.hardwareDepth.resource ='))
        wrapper = (BASE / 'FSRDCyberpunkFogDenoiseAccess.h').read_text()
        self.assertIn('CopySourceState = 0x800', wrapper)
        self.assertIn('Engine::InputReadState | (input.copySource ? CopySourceState : 0u)', wrapper)
        self.assertIn('host.Flush(input.image + Engine::FlushRva', wrapper)
        self.assertIn('host.Reenter(input.image + Engine::ReenterRva', wrapper)
        self.assertIn('host.RestorePso(input.list, input.originalPso)', wrapper)

    def test_pure_descriptor_contract_does_not_change_native_states(self):
        header = (BASE / 'FSRDCyberpunkFogDepthCopy.h').read_text()
        for forbidden in ('GetDesc(', 'ResourceBarrier(', 'AddRef(', 'RequestState(',
                          'Dispatch(', 'CopyTextureRegion(', 'reinterpret_cast', 'std::isfinite'):
            self.assertNotIn(forbidden, header)
        for required in ('flags == AllowDepthStencil', 'currentSrvFormat != R32Float',
                         'source COPY_SOURCE', 'destination COPY_DEST',
                         'zero destination', 'null source box', 'StateBefore'):
            self.assertIn(required, header)

    def test_actual_header_accepts_only_exact_whole_r32_cases(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('CXX required')
        harness = r'''
#include "FSRDCyberpunkFogDepthCopy.h"
#include <cassert>
#include <limits>
using namespace FSRD::CyberpunkFogDepthCopy;
struct Description {
    uint32_t Dimension=3; uint64_t Width=1280; uint32_t Height=720;
    uint16_t DepthOrArraySize=1, MipLevels=1;
    uint32_t Format=41;
    struct Sample {uint32_t Count=1,Quality=0;} SampleDesc;
    uint32_t Layout=0,Flags=0;
};
constexpr Description Default{};
static_assert(ClassifySource(Default,41,1280,720)==SourceKind::TypedR32Float);
static_assert(AdmitDestination(Default,1280,720));
static_assert(noexcept(ClassifySource(Default,41,1280,720)));
static_assert(noexcept(AdmitDestination(Default,1280,720)));
int main(){
    // Real90d8 source is format39/flags2; its authored view is format41.
    auto depth=Default;depth.Format=39;depth.Flags=2;
    assert(ClassifySource(depth,41,1280,720)==SourceKind::TypelessR32Depth);
    assert(!AdmitDestination(depth,1280,720));
    for(uint32_t format=0;format<256;++format){
        for(uint32_t flags=0;flags<256;++flags){
            auto d=Default;d.Format=format;d.Flags=flags;
            const bool typed=format==41 && (flags==0||flags==1||flags==4||flags==5);
            const bool typeless=format==39 && flags==2;
            const auto result=ClassifySource(d,41,1280,720);
            assert((result==SourceKind::TypedR32Float)==typed);
            assert((result==SourceKind::TypelessR32Depth)==typeless);
            assert((result==SourceKind::Refused)==(!typed&&!typeless));
            assert(AdmitDestination(d,1280,720)==(format==41&&flags==0));
        }
    }
    for(uint32_t bit=8;bit<32;++bit){
        auto d=Default;d.Flags=uint32_t(1)<<bit;
        assert(ClassifySource(d,41,1280,720)==SourceKind::Refused);
        assert(!AdmitDestination(d,1280,720));
        d=depth;d.Flags|=uint32_t(1)<<bit;
        assert(ClassifySource(d,41,1280,720)==SourceKind::Refused);
    }
    for(unsigned bad=0;bad<20;++bad){
        auto d=Default;uint32_t width=1280,height=720;
        switch(bad){
        case 0:d.Dimension=0;break;case 1:d.Dimension=1;break;
        case 2:d.Dimension=2;break;case 3:d.Dimension=4;break;
        case 4:d.Width=0;break;case 5:++d.Width;break;
        case 6:d.Width=std::numeric_limits<uint64_t>::max();break;
        case 7:d.Height=0;break;case 8:++d.Height;break;
        case 9:d.DepthOrArraySize=0;break;case 10:d.DepthOrArraySize=2;break;
        case 11:d.MipLevels=0;break;case 12:d.MipLevels=2;break;
        case 13:d.SampleDesc.Count=0;break;case 14:d.SampleDesc.Count=2;break;
        case 15:d.SampleDesc.Quality=1;break;case 16:d.Layout=1;break;
        case 17:width=0;break;case 18:height=0;break;
        case 19:width=MaxDimension+1;d.Width=width;break;
        }
        assert(ClassifySource(d,41,width,height)==SourceKind::Refused);
        assert(!AdmitDestination(d,width,height));
        d.Format=39;d.Flags=2;
        assert(ClassifySource(d,41,width,height)==SourceKind::Refused);
    }
    for(uint32_t view=0;view<256;++view){
        assert((ClassifySource(depth,view,1280,720)!=SourceKind::Refused)==(view==41));
        assert((ClassifySource(Default,view,1280,720)!=SourceKind::Refused)==(view==41));
    }
    auto d=Default;d.Width=1;d.Height=1;
    assert(AdmitDestination(d,1,1));
    d.Width=MaxDimension;d.Height=MaxDimension;
    assert(AdmitDestination(d,MaxDimension,MaxDimension));
    ++d.Height;
    assert(!AdmitDestination(d,MaxDimension,MaxDimension+1));
    // A partial active rectangle is intentionally refused, even though a color
    // texture might legally allow it: the supported depth copy is WHOLE only.
    assert(ClassifySource(depth,41,1279,720)==SourceKind::Refused);
    assert(ClassifySource(depth,41,1280,719)==SourceKind::Refused);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-fog-depth-copy-') as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(harness)
            for options in (['-O0'], ['-O3', '-ffast-math']):
                exe = Path(tmp) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *options,
                                '-I', str(BASE), str(source), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

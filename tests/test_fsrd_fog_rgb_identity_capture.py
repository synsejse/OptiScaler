"""Optional native post-identity/pre-Fog snapshot: actual capture code on mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_fsrd_fog_depth_capture as shared

ROOT, SOURCE, HEADER, body = shared.ROOT, shared.SOURCE, shared.HEADER, shared.body


class FogRgbIdentityCapture(unittest.TestCase):
    def test_optional_native_contract_and_completion_order(self):
        record = body('Record')
        start = record.index('if (layers.rgbIdentity.resource)')
        end = record.index('// Validate every optional entry')
        admission = record[start:end]
        for text in ('if (!keepAlive)', 'IsRayCopyTexture(', 'texture.state, scene.Width, scene.Height, false)',
                     'QueryInterface', 'if (!snapshotIdentity)', 'identity.Get() == snapshotIdentity.Get()',
                     'for (const auto& entry : batch->entries) distinct(entry)',
                     'distinct(*batch->boundCb12)', 'distinct(*batch->hardwareDepth)',
                     'for (const auto& entry : *batch->earlyGuides) distinct(entry)',
                     'for (const auto& entry : *batch->privateReset) distinct(entry)',
                     'distinct(*batch->exposureWords)', 'distinct(*batch->lightingT8)',
                     'for (const auto& entry : *batch->rayCopies) distinct(entry)', 'totalBytes > MaxBytes'):
            self.assertIn(text, admission)
        self.assertLess(end, record.index('AllocateReadback(device, entry)'))
        self.assertLess(record.index('if (!args->ticket)'), record.index('RecordCopy(list, entry)'))
        self.assertIn('RecordCopy(list, *batch->rgbIdentity)', record)
        for text in ('optiscaler.fsr_rr.fog_rgb_identity.v1', 'pre_fog_after_identity_rgb; caller_supplied',
                     'companion["identity_passed"] = false', 'not_performed; native comparison required',
                     'none; native RGBA float16 bits including unmodified alpha'):
            self.assertIn(text, record)
        writer = body('WriteWhenComplete')
        self.assertLess(writer.index('FSRDSubmission::Complete(args.ticket)'), writer.index('if (batch.rgbIdentity)'))
        self.assertLess(writer.index('WriteEntry(batch, *batch.rgbIdentity)'), writer.index('metadata["complete"] = true'))
        self.assertNotIn('rgbIdentity', body('RecordEarlyGuides'))
        self.assertIn('Texture rgbIdentity;', HEADER)
        self.assertIn('before remains the native', HEADER)
        self.assertIn('after remains AFTER the original Fog draw', HEADER)

    def test_compiled_actual_record_admission_alias_budget_and_failure_ownership(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for actual RGB identity capture tests')
        prefix, driver = shared.mock_fragments()
        driver = driver.replace('assert(batch&&batch->keepAlive)', 'assert(batch)')
        driver = driver.replace('++copies;',
                                '++copies;if(failAfterCopy==copies)throw std::runtime_error("mock partial copy");')
        structs = SOURCE[SOURCE.index('struct Entry'):SOURCE.index('enum class RequestKind')]
        functions = '\n'.join(signature + body(name) for name, signature in (
            ('IsExposureWordsTexture', 'bool IsExposureWordsTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state) noexcept'),
            ('IsRayCopyTexture', 'bool IsRayCopyTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state,UINT width,UINT height,bool rayHit) noexcept'),
            ('PrepareEntry', 'void PrepareEntry(ID3D12Device* device,Entry& entry,bool boundCb12=false,bool guideAlbedo=false,bool exposureWords=false,bool rayHit=false)'),
            ('Describe', 'Json Describe(const Entry& entry)')))
        layers = HEADER[HEADER.index('struct Layers'):HEADER.index('struct Status')]
        harness = prefix + '\n#include <algorithm>\nunsigned failAfterCopy=0;\n' + structs + functions + driver + layers + r'''
bool Record(ID3D12Device* device,ID3D12GraphicsCommandList* list,const Layers& layers,
 const std::string& provenanceJson,const std::shared_ptr<void>& keepAlive={})
''' + body('Record') + r'''
int main(){
 ID3D12Device device;ID3D12GraphicsCommandList list;ID3D12Resource r[12];
 Layers layers;auto owner=std::make_shared<int>(1);
 auto setup=[&](UINT width=1280,UINT height=720){
  allocations=copies=retains=workers=moduleRefs=failAfterCopy=0;failRetain=false;
  auto& registry=GetRegistry(RequestKind::FogLayers);registry.requested=true;
  registry.pending.reset();registry.ticket.reset();layers={};
  auto& early=GetRegistry(RequestKind::EarlyGuides);early.requested=true;early.pending.reset();early.ticket.reset();
  for(unsigned i=0;i<12;++i){r[i].desc={};r[i].desc.Width=width;r[i].desc.Height=height;
   r[i].identity=&r[i];r[i].failIdentity=false;}
  layers.before={&r[0],0,10,0xc0};layers.after={&r[1],0,10,0xc0};
  r[2].desc.Format=2;layers.authored={&r[2],0,2,0xc0};
  r[3].desc.Format=3;r[3].desc.Width=5;r[3].desc.Height=1;layers.boundCb12={&r[3],0,3,0xc0};
  for(unsigned i=0;i<3;++i){r[i+4].desc.Format=i<2?28:10;layers.earlyGuides[i]={&r[i+4],0,r[i+4].desc.Format,0xc0};}
  r[7].desc.Format=41;layers.hardwareDepth={&r[7],0,41,0xc0};
  for(unsigned i=0;i<3;++i)layers.privateReset[i]={&r[i+8],0,10,0xc0};
  layers.rgbIdentity={&r[11],0,10,0xc0};
 };
 auto run=[&](){return Record(&device,&list,layers,"{}",owner);};
 setup();assert(run()&&allocations==12&&copies==12&&retains==1&&workers==1&&!moduleRefs);
 auto batch=GetRegistry(RequestKind::FogLayers).pending;
 assert(batch&&batch->rgbIdentity&&batch->keepAlive==owner);
 const auto& entry=*batch->rgbIdentity;const auto& metadata=batch->metadata["companions"].back();
 assert(entry.role==std::string("rgb_identity")&&entry.filename=="rgb_identity.rgba16f");
 assert(entry.componentType==std::string("float16")&&entry.pixelBytes==8&&entry.channels==std::string("RGBA"));
 assert(batch->metadata["layers"].size()==3&&batch->metadata["companions"].size()==9);
 assert(metadata["schema"]=="optiscaler.fsr_rr.fog_rgb_identity.v1"&&metadata["identity_passed"]==false);
 assert(metadata["identity_validation"]=="not_performed; native comparison required");
 assert(metadata["snapshot_stage"]=="pre_fog_after_identity_rgb; caller_supplied");
 assert(metadata["state_restored"]==0xc0&&metadata["file_bytes"]==1280*720*8);
 // The original three source objects/roles retain their meaning and ordering.
 assert(batch->entries[0].source.resource.Get()==&r[0]&&batch->entries[0].role==std::string("scene_before"));
 assert(batch->entries[1].source.resource.Get()==&r[1]&&batch->entries[1].role==std::string("scene_after"));
 assert(batch->entries[2].source.resource.Get()==&r[2]&&batch->entries[2].role==std::string("authored_fog"));
 assert(GetRegistry(RequestKind::EarlyGuides).requested&&!GetRegistry(RequestKind::EarlyGuides).ticket);
 assert(!run()&&copies==12);batch.reset();
 // Different interface pointers still cannot hide aliases to any present companion.
 for(unsigned other=0;other<11;++other){
  setup();r[11].identity=&r[other];assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 for(unsigned field=0;field<18;++field){
  setup();auto& t=layers.rgbIdentity;auto& d=r[11].desc;
  switch(field){
   case 0:t.subresource=1;break;case 1:t.viewFormat=9;break;case 2:d.Format=9;break;
   case 3:d.Dimension=1;break;case 4:d.MipLevels=2;break;case 5:d.DepthOrArraySize=2;break;
   case 6:d.SampleDesc.Count=2;break;case 7:d.SampleDesc.Quality=1;break;
   case 8:t.state=0x8c0;break;case 9:d.Width=1279;break;case 10:d.Height=719;break;
   case 11:r[11].failIdentity=true;break;case 12:t.state=0xe0;break;
   case 13:d.Width=0;break;case 14:d.Height=0;break;case 15:r[11].identity=nullptr;break;
   case 16:d.SampleDesc.Count=0;break;case 17:d.MipLevels=0;break;
  }
  assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 // RGB companion alone still requires producer ownership, independently of other optional roles.
 setup();layers.privateReset={};layers.hardwareDepth={};layers.earlyGuides={};layers.boundCb12={};
 assert(!Record(&device,&list,layers,"{}",{})&&!allocations&&!copies&&!retains);
 setup(4096,2048);layers.privateReset={};layers.hardwareDepth={};layers.earlyGuides={};layers.boundCb12={};
 // Base three consume exactly256MiB; reject final identity entry before ANY allocation.
 assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 setup();failRetain=true;assert(!run()&&allocations==12&&retains==1&&!copies&&!workers&&!moduleRefs);
 assert(!GetRegistry(RequestKind::FogLayers).pending&&!GetRegistry(RequestKind::FogLayers).ticket);
 // A partial-recording failure cannot release the earlier command owners.
 setup();failAfterCopy=12;assert(!run()&&copies==12&&retains==1&&!workers&&!moduleRefs);
 assert(GetRegistry(RequestKind::FogLayers).pending->keepAlive==owner&&GetRegistry(RequestKind::FogLayers).ticket);
 setup();layers.rgbIdentity={};assert(run()&&allocations==11&&copies==11&&!GetRegistry(RequestKind::FogLayers).pending->rgbIdentity);
 setup();layers.rgbIdentity={};layers.privateReset={};layers.hardwareDepth={};layers.earlyGuides={};layers.boundCb12={};
 assert(Record(&device,&list,layers,"{}",{})&&allocations==3&&copies==3);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-rgb-identity-capture-') as directory:
            source, binary = Path(directory) / 'capture.cpp', Path(directory) / 'capture'
            source.write_text(harness)
            for flags in (('-O0',), ('-O3', '-ffast-math')):
                result = subprocess.run([compiler, '-std=c++20', *flags, '-Wall', '-Wextra',
                                         '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                subprocess.run([str(binary)], check=True)

    def test_actual_native_writer_preserves_alpha_nonfinite_and_signed_zero_bits(self):
        # The new entry uses this unchanged bytewise WriteEntry, never floating math.
        shared.shared.RayCopyCapture().test_compiled_actual_writer_preserves_scalar_and_rgba_bits()


if __name__ == '__main__':
    unittest.main()

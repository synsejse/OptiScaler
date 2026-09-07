"""Private RESET output capture; actual production admission/recording on mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_fsrd_fog_depth_capture as shared

ROOT, SOURCE, HEADER, body = shared.ROOT, shared.SOURCE, shared.HEADER, shared.body


class PrivateResetCapture(unittest.TestCase):
    def test_required_native_owner_alias_budget_and_fence_contract(self):
        record = body('Record')
        start = record.index('const auto privateResetCount')
        end = record.index('// Validate every optional entry')
        admission = record[start:end]
        for text in ('privateResetCount != 3', 'if (!keepAlive)', 'IsRayCopyTexture(',
                     'scene.Width, scene.Height, false)', 'QueryInterface',
                     'for (const auto& entry : batch->entries) distinct(entry)',
                     'distinct(*batch->boundCb12)', 'distinct(*batch->hardwareDepth)',
                     'for (const auto& entry : *batch->earlyGuides) distinct(entry)',
                     'identities[previous].Get() == identities[i].Get()',
                     'totalBytes > MaxBytes'):
            self.assertIn(text, admission)
        self.assertLess(end, record.index('AllocateReadback(device, entry)'))
        self.assertLess(record.index('if (!args->ticket)'), record.index('RecordCopy(list, entry)'))
        self.assertIn('batch->privateReset', record)
        for value in ('private_reset_radiance', 'private_reset_denoised', 'private_reset_composed',
                      'optiscaler.fsr_rr.private_reset_output.v1', 'independent_RESET_diagnostic',
                      'companion["live_correction"] = false'):
            self.assertIn(value, record)
        writer = body('WriteWhenComplete')
        self.assertLess(writer.index('FSRDSubmission::Complete(args.ticket)'), writer.index('if (batch.privateReset)'))
        self.assertLess(writer.index('if (batch.privateReset)'), writer.index('metadata["complete"] = true'))
        self.assertNotIn('privateReset', body('RecordEarlyGuides'))
        self.assertIn('std::array<Texture, 3> privateReset;', HEADER)

    def test_compiled_actual_record_three_outputs_and_all_failure_guards(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler: self.skipTest('Set CXX for actual private RESET capture tests')
        prefix, driver = shared.mock_fragments()
        # The shared driver was authored for mandatory-owner standalone guides.
        # Ordinary three-layer Fog historically allows no extra caller owner.
        driver = driver.replace('assert(batch&&batch->keepAlive)', 'assert(batch)')
        structs = SOURCE[SOURCE.index('struct Entry'):SOURCE.index('enum class RequestKind')]
        functions = '\n'.join(signature + body(name) for name, signature in (
            ('IsExposureWordsTexture', 'bool IsExposureWordsTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state) noexcept'),
            ('IsRayCopyTexture', 'bool IsRayCopyTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state,UINT width,UINT height,bool rayHit) noexcept'),
            ('PrepareEntry', 'void PrepareEntry(ID3D12Device* device,Entry& entry,bool boundCb12=false,bool guideAlbedo=false,bool exposureWords=false,bool rayHit=false)'),
            ('Describe', 'Json Describe(const Entry& entry)')))
        layers = HEADER[HEADER.index('struct Layers'):HEADER.index('struct Status')]
        harness = prefix + '\n#include <algorithm>\n' + structs + functions + driver + layers + r'''
bool Record(ID3D12Device* device,ID3D12GraphicsCommandList* list,const Layers& layers,
 const std::string& provenanceJson,const std::shared_ptr<void>& keepAlive={})
''' + body('Record') + r'''
int main(){
 ID3D12Device device;ID3D12GraphicsCommandList list;ID3D12Resource r[11];
 Layers layers;auto owner=std::make_shared<int>(1);
 auto setup=[&](UINT width=1280,UINT height=720){
  allocations=copies=retains=workers=moduleRefs=0;failRetain=false;
  auto& registry=GetRegistry(RequestKind::FogLayers);registry.requested=true;
  registry.pending.reset();registry.ticket.reset();layers={};
  for(unsigned i=0;i<11;++i){r[i].desc={};r[i].desc.Width=width;r[i].desc.Height=height;
   r[i].identity=&r[i];r[i].failIdentity=false;}
  layers.before={&r[0],0,10,0xc0};layers.after={&r[1],0,10,0xc0};
  r[2].desc.Format=2;layers.authored={&r[2],0,2,0xc0};
  r[3].desc.Format=3;r[3].desc.Width=5;r[3].desc.Height=1;layers.boundCb12={&r[3],0,3,0xc0};
  for(unsigned i=0;i<3;++i){r[i+4].desc.Format=i<2?28:10;layers.earlyGuides[i]={&r[i+4],0,r[i+4].desc.Format,0xc0};}
  r[7].desc.Format=41;layers.hardwareDepth={&r[7],0,41,0xc0};
  for(unsigned i=0;i<3;++i)layers.privateReset[i]={&r[i+8],0,10,0xc0};
 };
 auto run=[&](){return Record(&device,&list,layers,"{}",owner);};
 setup();assert(run()&&allocations==11&&copies==11&&retains==1&&workers==1&&!moduleRefs);
 auto batch=GetRegistry(RequestKind::FogLayers).pending;
 assert(batch&&batch->privateReset&&batch->keepAlive==owner);
 constexpr const char* roles[]={"private_reset_radiance","private_reset_denoised","private_reset_composed"};
 assert(batch->metadata["layers"].size()==3&&batch->metadata["companions"].size()==8);
 for(unsigned i=0;i<3;++i){
  const auto& e=(*batch->privateReset)[i];const auto& m=batch->metadata["companions"][5+i];
  assert(e.role==std::string(roles[i])&&e.filename==std::string(roles[i])+".rgba16f");
  assert(e.componentType==std::string("float16")&&e.pixelBytes==8&&e.channels==std::string("RGBA"));
  assert(m["schema"]=="optiscaler.fsr_rr.private_reset_output.v1"&&m["output_index"]==i);
  assert(m["intended_mode"]=="independent_RESET_diagnostic"&&m["live_correction"]==false);
  assert(m["state_restored"]==0xc0&&m["file_bytes"]==1280*720*8);
 }
 assert(!run()&&copies==11);batch.reset();
 // Canonical identity aliases are forbidden even if different interface pointers.
 for(unsigned i=0;i<3;++i)for(unsigned other=0;other<11;++other){
  if(other==i+8)continue;
  setup();r[i+8].identity=&r[other];assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 for(unsigned i=0;i<3;++i)for(unsigned field=0;field<17;++field){
  setup();auto& t=layers.privateReset[i];auto& d=r[i+8].desc;
  switch(field){
   case 0:t.subresource=1;break;case 1:t.viewFormat=9;break;case 2:d.Format=9;break;
   case 3:d.Dimension=1;break;case 4:d.MipLevels=2;break;case 5:d.DepthOrArraySize=2;break;
   case 6:d.SampleDesc.Count=2;break;case 7:d.SampleDesc.Quality=1;break;
   case 8:t.state=0x8c0;break;case 9:d.Width=1279;break;case 10:d.Height=719;break;
   case 11:r[i+8].failIdentity=true;break;case 12:t.state=0xe0;break;
   case 13:d.Width=0;break;case 14:d.Height=0;break;case 15:r[i+8].identity=nullptr;break;
   case 16:t.resource=nullptr;break;
  }
  assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 setup();assert(!Record(&device,&list,layers,"{}",{})&&!allocations&&!copies&&!retains);
 setup(4096,1536);layers.boundCb12={};layers.earlyGuides={};layers.hardwareDepth={};
 // Native scene layers192MiB; two outputs reach288MiB: all allocation still refused.
 assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 setup();failRetain=true;assert(!run()&&allocations==11&&retains==1&&!copies&&!workers&&!moduleRefs);
 assert(!GetRegistry(RequestKind::FogLayers).pending&&!GetRegistry(RequestKind::FogLayers).ticket);
 setup();layers.privateReset={};assert(run()&&allocations==8&&copies==8&&!GetRegistry(RequestKind::FogLayers).pending->privateReset);
 setup();layers.privateReset={};layers.hardwareDepth={};layers.earlyGuides={};layers.boundCb12={};
 assert(Record(&device,&list,layers,"{}",{})&&allocations==3&&copies==3);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-private-reset-capture-') as directory:
            source, binary = Path(directory) / 'capture.cpp', Path(directory) / 'capture'
            source.write_text(harness)
            for flags in (('-O0',), ('-O3', '-ffast-math')):
                result = subprocess.run([compiler, '-std=c++20', *flags, '-Wall', '-Wextra',
                                         '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                subprocess.run([str(binary)], check=True)

    def test_actual_raw_writer_still_preserves_rgba_nonfinite_and_signed_zero(self):
        # Same unchanged native WriteEntry path, with source ownership retained by Batch.
        shared.shared.RayCopyCapture().test_compiled_actual_writer_preserves_scalar_and_rgba_bits()


if __name__ == '__main__':
    unittest.main()

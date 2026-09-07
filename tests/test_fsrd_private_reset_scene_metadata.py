"""Explicit one-shot scene-write evidence must not relabel default private captures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_fsrd_fog_depth_capture as shared

ROOT, SOURCE, HEADER, body = shared.ROOT, shared.SOURCE, shared.HEADER, shared.body


class PrivateResetSceneMetadata(unittest.TestCase):
    def test_default_off_and_scene_semantics_are_metadata_only(self):
        self.assertIn('bool privateResetSceneWrite = false;', HEADER)
        self.assertIn('this boolean grants no scene-write authority or quality proof', HEADER)
        record = body('Record')
        guard = record.index('if (layers.privateResetSceneWrite && (privateResetCount != 3 || layers.rgbIdentity.resource))')
        self.assertLess(guard, record.index('AllocateReadback(device, entry)'))
        for value in ('independent_RESET_scene_control', 'one_shot_only; caller_supplied',
                      'not_verified_by_readback_helper', 'companion["live_correction"] = true'):
            self.assertIn(value, record)
        self.assertIn('companion["intended_mode"] = "independent_RESET_diagnostic"', record)
        self.assertIn('companion["live_correction"] = false', record)
        self.assertNotIn('privateResetSceneWrite', body('RecordEarlyGuides'))
        self.assertNotIn('privateResetSceneWrite', body('RecordCopy'))
        self.assertNotIn('privateResetSceneWrite', body('WriteEntry'))
        self.assertLess(record.index('if (!args->ticket)'), record.index('RecordCopy(list, entry)'))

    def test_actual_record_default_compatibility_required_outputs_and_exclusive_identity(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler: self.skipTest('Set CXX for actual scene RESET metadata tests')
        prefix, driver = shared.mock_fragments()
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
 ID3D12Device device;ID3D12GraphicsCommandList list;ID3D12Resource r[7];
 Layers layers;auto owner=std::make_shared<int>(1);
 auto setup=[&](bool scene=false,UINT width=1280,UINT height=720){
  allocations=copies=retains=workers=moduleRefs=0;failRetain=false;
  auto& registry=GetRegistry(RequestKind::FogLayers);registry.requested=true;
  registry.pending.reset();registry.ticket.reset();layers={};
  assert(!layers.privateResetSceneWrite);layers.privateResetSceneWrite=scene;
  for(unsigned i=0;i<7;++i){r[i].desc={};r[i].desc.Width=width;r[i].desc.Height=height;
   r[i].identity=&r[i];r[i].failIdentity=false;}
  layers.before={&r[0],0,10,0xc0};layers.after={&r[1],0,10,0xc0};
  r[2].desc.Format=2;layers.authored={&r[2],0,2,0xc0};
  for(unsigned i=0;i<3;++i)layers.privateReset[i]={&r[i+3],0,10,0xc0};
 };
 auto run=[&](){return Record(&device,&list,layers,"{}",owner);};
 setup();assert(run()&&allocations==6&&copies==6&&retains==1&&workers==1&&!moduleRefs);
 const Json defaultMetadata=GetRegistry(RequestKind::FogLayers).pending->metadata;
 for(const auto& m:defaultMetadata["companions"]){
  assert(m["schema"]=="optiscaler.fsr_rr.private_reset_output.v1");
  assert(m["intended_mode"]=="independent_RESET_diagnostic"&&m["live_correction"]==false);
  assert(!m.contains("scene_write_scope")&&!m.contains("scene_write_proof"));
 }
 setup(true);assert(run()&&allocations==6&&copies==6&&retains==1&&workers==1&&!moduleRefs);
 auto batch=GetRegistry(RequestKind::FogLayers).pending;
 assert(batch->keepAlive==owner&&batch->privateReset&&!batch->rgbIdentity);
 Json sceneMetadata=batch->metadata;
 assert(sceneMetadata["companions"].size()==3&&sceneMetadata["layers"].size()==3);
 for(unsigned i=0;i<3;++i){
  auto& m=sceneMetadata["companions"][i];
  assert(m["output_index"]==i&&m["schema"]=="optiscaler.fsr_rr.private_reset_output.v1");
  assert(m["intended_mode"]=="independent_RESET_scene_control"&&m["live_correction"]==true);
  assert(m["scene_write_scope"]=="one_shot_only; caller_supplied");
  assert(m["scene_write_proof"]=="not_verified_by_readback_helper");
  // Exactly the labels change; no file/format/state/pixel/budget/source substitution.
  m["intended_mode"]="independent_RESET_diagnostic";m["live_correction"]=false;
  m.erase("scene_write_scope");m.erase("scene_write_proof");
 }
 assert(sceneMetadata==defaultMetadata);assert(!run()&&copies==6);batch.reset();
 for(unsigned count=0;count<3;++count){
  setup(true);for(unsigned i=count;i<3;++i)layers.privateReset[i]={};
  assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 setup(true);layers.rgbIdentity={&r[6],0,10,0xc0};
 assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 setup(false);layers.rgbIdentity={&r[6],0,10,0xc0};
 assert(run()&&allocations==7&&copies==7); // Existing private+identity storage remains unchanged.
 setup(true);assert(!Record(&device,&list,layers,"{}",{})&&!allocations&&!copies&&!retains);
 setup(true);r[5].identity=&r[0];assert(!run()&&!allocations&&!copies&&!retains);
 setup(true);r[5].desc.Format=9;assert(!run()&&!allocations&&!copies&&!retains);
 setup(true,4096,2048);assert(!run()&&!allocations&&!copies&&!retains); // Existing shared256MiB cap.
 setup(true);failRetain=true;assert(!run()&&allocations==6&&retains==1&&!copies&&!workers&&!moduleRefs);
 setup(false);layers.privateReset={};assert(Record(&device,&list,layers,"{}",{})&&copies==3);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-scene-reset-metadata-') as directory:
            source, binary = Path(directory) / 'capture.cpp', Path(directory) / 'capture'
            source.write_text(harness)
            for flags in (('-O0',), ('-O3', '-ffast-math')):
                compiled = subprocess.run([compiler, '-std=c++20', *flags, '-Wall', '-Wextra',
                                           '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)],
                                          capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__': unittest.main()

"""Final temporal-window metadata is explicit caller evidence, not a new capture path."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_fsrd_fog_depth_capture as shared

ROOT, SOURCE, HEADER, body = shared.ROOT, shared.SOURCE, shared.HEADER, shared.body


class TemporalCaptureMetadata(unittest.TestCase):
    def test_default_mode_and_no_recording_or_worker_branch(self):
        self.assertIn('PrivateOutputMode privateOutputMode = PrivateOutputMode::IndependentReset;', HEADER)
        self.assertIn('TemporalWindowFinalSceneControl32 = 1', HEADER)
        self.assertIn('Does not authorize or verify any draw', HEADER)
        record = body('Record')
        for guard in ('unknown private denoiser output metadata mode',
                      'temporalFinalScene && (!layers.privateResetSceneWrite || privateResetCount != 3 || layers.rgbIdentity.resource)'):
            self.assertLess(record.index(guard), record.index('AllocateReadback(device, entry)'))
        self.assertIn('"optiscaler.fsr_rr.temporal_window_output.v1"', record)
        self.assertIn('"caller_supplied; not_verified_by_readback_helper"', record)
        for function in ('RecordEarlyGuides', 'RecordCopy', 'WriteEntry', 'WriteWhenComplete'):
            self.assertNotIn('privateOutputMode', body(function))
            self.assertNotIn('temporalFinalScene', body(function))
        self.assertLess(record.index('if (!args->ticket)'), record.index('RecordCopy(list, entry)'))

    def test_actual_record_final_metadata_legacy_equality_and_fail_closed_admission(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for actual temporal capture metadata tests')
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
 ID3D12Device device;ID3D12GraphicsCommandList list;ID3D12Resource resources[7];
 Layers layers;auto owner=std::make_shared<int>(1);
 using Mode=Layers::PrivateOutputMode;
 auto setup=[&](Mode mode=Mode::IndependentReset,bool scene=false,UINT width=1280,UINT height=720){
  allocations=copies=retains=workers=moduleRefs=0;failRetain=false;
  auto& registry=GetRegistry(RequestKind::FogLayers);registry.requested=true;
  registry.pending.reset();registry.ticket.reset();layers={};
  assert(layers.privateOutputMode==Mode::IndependentReset&&!layers.privateResetSceneWrite);
  layers.privateOutputMode=mode;layers.privateResetSceneWrite=scene;
  for(unsigned i=0;i<7;++i){resources[i].desc={};resources[i].desc.Width=width;resources[i].desc.Height=height;
   resources[i].identity=&resources[i];resources[i].failIdentity=false;}
  layers.before={&resources[0],0,10,0xc0};layers.after={&resources[1],0,10,0xc0};
  resources[2].desc.Format=2;layers.authored={&resources[2],0,2,0xc0};
  for(unsigned i=0;i<3;++i)layers.privateReset[i]={&resources[i+3],0,10,0xc0};
 };
 auto run=[&](){return Record(&device,&list,layers,"{}",owner);};
 setup();assert(run()&&allocations==6&&copies==6&&retains==1&&workers==1&&!moduleRefs);
 for(const auto& m:GetRegistry(RequestKind::FogLayers).pending->metadata["companions"]){
  assert(m["schema"]=="optiscaler.fsr_rr.private_reset_output.v1"&&m["live_correction"]==false);
  assert(m["intended_mode"]=="independent_RESET_diagnostic"&&!m.contains("temporal_window"));
 }
 setup(Mode::IndependentReset,true);assert(run());
 const Json oneShot=GetRegistry(RequestKind::FogLayers).pending->metadata;
 setup(Mode::TemporalWindowFinalSceneControl32,true);assert(run());
 assert(allocations==6&&copies==6&&retains==1&&workers==1&&!moduleRefs);
 auto batch=GetRegistry(RequestKind::FogLayers).pending;
 assert(batch->keepAlive==owner&&batch->privateReset&&!batch->rgbIdentity);
 Json temporal=batch->metadata;assert(temporal["companions"].size()==3&&temporal["layers"].size()==3);
 for(unsigned i=0;i<3;++i){auto& m=temporal["companions"][i];
  assert(m["output_index"]==i&&m["schema"]=="optiscaler.fsr_rr.temporal_window_output.v1");
  assert(m["intended_mode"]=="temporal_window_final_scene_control"&&m["live_correction"]==true);
  assert(m["scene_write_scope"]=="final_frame_of_bounded_32_frame_window; caller_supplied");
  assert(m["scene_write_proof"]=="not_verified_by_readback_helper");
  const auto& window=m["temporal_window"];
  assert(window["frame_count"]==32&&window["captured_frame_ordinal"]==32);
  assert(window["first_frame_reset"]==true&&window["subsequent_frames_reset"]==false);
  assert(window["proof"]=="caller_supplied; not_verified_by_readback_helper");
  // Metadata is the ONLY difference: role/file/native layout/alpha/state/bytes
  // and immutable scene-layer identities stay equal to the existing scene case.
  for(const char* key:{"schema","intended_mode","scene_write_scope","input_provenance"})
   m[key]=oneShot["companions"][i][key];
  m.erase("temporal_window");
 }
 assert(temporal==oneShot);assert(!run()&&copies==6);batch.reset();
 for(unsigned count=0;count<3;++count){setup(Mode::TemporalWindowFinalSceneControl32,true);
  for(unsigned i=count;i<3;++i)layers.privateReset[i]={};
  assert(!run()&&!allocations&&!copies&&!retains&&!workers);}
 setup(Mode::TemporalWindowFinalSceneControl32,false);assert(!run()&&!allocations&&!copies&&!retains);
 setup(Mode::TemporalWindowFinalSceneControl32,true);layers.rgbIdentity={&resources[6],0,10,0xc0};
 assert(!run()&&!allocations&&!copies&&!retains);
 for(int unknown:{-1,2,32,255}){setup(static_cast<Mode>(unknown),true);
  assert(!run()&&!allocations&&!copies&&!retains);
  setup(static_cast<Mode>(unknown),false);layers.privateReset={};
  assert(!run()&&!allocations&&!copies&&!retains);}
 setup(Mode::TemporalWindowFinalSceneControl32,true);
 assert(!Record(&device,&list,layers,"{}",{})&&!allocations&&!copies&&!retains);
 setup(Mode::TemporalWindowFinalSceneControl32,true);resources[5].identity=&resources[0];
 assert(!run()&&!allocations&&!copies&&!retains);
 setup(Mode::TemporalWindowFinalSceneControl32,true);layers.privateReset[2].state=0x8c0;
 assert(!run()&&!allocations&&!copies&&!retains);
 setup(Mode::TemporalWindowFinalSceneControl32,true,4096,2048);
 assert(!run()&&!allocations&&!copies&&!retains); // Unchanged aggregate256MiB cap.
 setup(Mode::TemporalWindowFinalSceneControl32,true);failRetain=true;
 assert(!run()&&allocations==6&&retains==1&&!copies&&!workers&&!moduleRefs);
 setup();layers.privateReset={};assert(Record(&device,&list,layers,"{}",{})&&copies==3);
 setup();layers.rgbIdentity={&resources[6],0,10,0xc0};assert(run()&&copies==7);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-temporal-capture-metadata-') as directory:
            source, binary = Path(directory) / 'capture.cpp', Path(directory) / 'capture'
            source.write_text(harness)
            for flags in (('-O0',), ('-O3', '-ffast-math')):
                compiled = subprocess.run([compiler, '-std=c++20', *flags, '-Wall', '-Wextra',
                                           '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)],
                                          capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

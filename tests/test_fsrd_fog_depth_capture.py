"""Fog-only native depth companion; reuse the audited readback mock environment."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_fsrd_ray_copy_capture as shared

ROOT, SOURCE, HEADER, body = shared.ROOT, shared.SOURCE, shared.HEADER, shared.body


def mock_fragments():
    # Reuse only the two literal platform/driver mocks from the existing capture
    # test, not its tested functions or assertions. Actual current production
    # PrepareEntry/Describe/Record bodies are independently compiled below.
    module = ast.parse(Path(shared.__file__).read_text())
    test = next(node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)
                and node.name == "test_compiled_actual_prepare_and_record_contracts")
    assignment = next(node for node in test.body if isinstance(node, ast.Assign)
                      and any(isinstance(target, ast.Name) and target.id == "harness" for target in node.targets))

    def strings(node):
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            return strings(node.left) + strings(node.right)
        return [node.value] if isinstance(node, ast.Constant) and isinstance(node.value, str) else []

    pieces = strings(assignment.value)
    assert pieces[0].startswith("\n#include <array>")
    assert pieces[1].startswith("\nenum class RequestKind")
    driver = pieces[1].split("bool RecordEarlyGuides(", 1)[0].replace(
        "Timestamp(const char*)", 'Timestamp(const char* = "fog")')
    driver = driver.replace("assert(GetRegistry().ticket)", "assert(GetRegistry(RequestKind::FogLayers).ticket)")
    return pieces[0], driver


class FogDepthCapture(unittest.TestCase):
    def test_private_depth_contract_budget_retention_and_worker(self):
        record = body("Record")
        first_allocation = record.index("AllocateReadback(device, entry)")
        for exact in ("if (!keepAlive)", "depthIdentity.Get()", "if (!depthIdentity)",
                      "identity.Get() == depthIdentity.Get()", "for (const auto& entry : batch->entries) distinct(entry)",
                      "distinct(*batch->boundCb12)", "for (const auto& entry : *batch->earlyGuides) distinct(entry)",
                      "PrepareEntry(device, *batch->hardwareDepth, false, false, false, true)",
                      "depth.Width != scene.Width || depth.Height != scene.Height",
                      "fog capture including hardware depth exceeds the shared256MiB readback limit"):
            self.assertLess(record.index(exact), first_allocation)
        self.assertLess(record.index("FSRDSubmission::Retain(device, list, batch)"), record.index("if (!args->ticket)"))
        self.assertLess(record.index("if (!args->ticket)"), record.index("RecordCopy(list, entry)"))
        self.assertIn("RecordCopy(list, *batch->hardwareDepth)", record)
        self.assertNotIn("hardwareDepth", body("RecordEarlyGuides"))
        for text in ('"optiscaler.fsr_rr.fog_hardware_depth.v1"', '"original_fog_depth_use; caller_supplied"',
                     '"none; native hardware-depth R float32 bits, no linearization"',
                     '"cross_capture_frame_relation"] = "not_asserted"'):
            self.assertIn(text, record)
        writer = body("WriteWhenComplete")
        self.assertLess(writer.index("FSRDSubmission::Complete(args.ticket)"), writer.index("if (batch.hardwareDepth)"))
        self.assertLess(writer.index("WriteEntry(batch, *batch.hardwareDepth)"), writer.index('metadata["complete"] = true'))
        self.assertIn("Texture hardwareDepth;", HEADER)

    def test_compiled_actual_fog_record_and_scalar_validation(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile actual Fog capture functions")
        prefix, driver = mock_fragments()
        structs = SOURCE[SOURCE.index("struct Entry"):SOURCE.index("enum class RequestKind")]
        functions = "\n".join(signature + body(name) for name, signature in (
            ("IsExposureWordsTexture", "bool IsExposureWordsTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state) noexcept"),
            ("IsRayCopyTexture", "bool IsRayCopyTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state,UINT width,UINT height,bool rayHit) noexcept"),
            ("PrepareEntry", "void PrepareEntry(ID3D12Device* device,Entry& entry,bool boundCb12=false,bool guideAlbedo=false,bool exposureWords=false,bool rayHit=false)"),
            ("Describe", "Json Describe(const Entry& entry)")))
        layers = HEADER[HEADER.index("struct Layers"):HEADER.index("struct Status")]
        harness = prefix + "\n#include <algorithm>\n" + structs + functions + driver + layers + r'''
bool Record(ID3D12Device* device,ID3D12GraphicsCommandList* list,const Layers& layers,
 const std::string& provenanceJson,const std::shared_ptr<void>& keepAlive={})
''' + body("Record") + r'''
int main(){
 ID3D12Device device;ID3D12GraphicsCommandList list;ID3D12Resource r[8];
 Layers layers;auto owner=std::make_shared<int>(1);
 auto setup=[&](UINT width=1280,UINT height=720){
  allocations=copies=retains=workers=moduleRefs=0;failRetain=false;
  auto& registry=GetRegistry(RequestKind::FogLayers);registry.requested=true;
  registry.pending.reset();registry.ticket.reset();layers={};
  auto& early=GetRegistry(RequestKind::EarlyGuides);early.requested=true;early.pending.reset();early.ticket.reset();
  for(unsigned i=0;i<8;++i){r[i].desc={};r[i].desc.Width=width;r[i].desc.Height=height;
   r[i].identity=&r[i];r[i].failIdentity=false;}
  layers.before={&r[0],0,10,0xc0};layers.after={&r[1],0,10,0xc0};
  r[2].desc.Format=2;layers.authored={&r[2],0,2,0xc0};
  r[3].desc.Format=3;r[3].desc.Width=5;r[3].desc.Height=1;layers.boundCb12={&r[3],0,3,0xc0};
  for(unsigned i=0;i<3;++i){r[i+4].desc.Format=i<2?28:10;layers.earlyGuides[i]={&r[i+4],0,r[i+4].desc.Format,0xc0};}
  r[7].desc.Format=41;layers.hardwareDepth={&r[7],0,41,0xc0};
 };
 auto run=[&](){return Record(&device,&list,layers,"{}",owner);};
 setup();assert(run()&&allocations==8&&copies==8&&retains==1&&workers==1&&!moduleRefs);
 auto batch=GetRegistry(RequestKind::FogLayers).pending;assert(batch&&batch->hardwareDepth);
 const auto& e=*batch->hardwareDepth;
 assert(e.filename=="hardware_depth.r32f"&&e.componentType==std::string("float32")&&e.channels==std::string("R"));
 assert(e.pixelBytes==4&&Describe(e)["file_bytes"]==1280*720*4);
 assert(batch->metadata["layers"].size()==3&&batch->metadata["companions"].size()==5);
 assert(batch->metadata["companions"][4]["schema"]=="optiscaler.fsr_rr.fog_hardware_depth.v1");
 assert(batch->metadata["companions"][4]["cross_capture_frame_relation"]=="not_asserted");
 assert(!run()&&copies==8);
 for(unsigned other=0;other<7;++other){
  setup();r[7].identity=&r[other];assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 for(unsigned field=0;field<16;++field){
  setup();auto& t=layers.hardwareDepth;auto& d=r[7].desc;
  switch(field){
   case 0:t.subresource=1;break;case 1:t.viewFormat=39;break;case 2:d.Format=39;break;
   case 3:d.Dimension=1;break;case 4:d.MipLevels=2;break;case 5:d.DepthOrArraySize=2;break;
   case 6:d.SampleDesc.Count=2;break;case 7:d.SampleDesc.Quality=1;break;
   case 8:t.state=0x8c0;break;case 9:d.Width=1279;break;case 10:d.Height=719;break;
   case 11:r[7].failIdentity=true;break;case 12:t.state=0xe0;break;
   case 13:d.Width=0;break;case 14:d.Height=0;break;case 15:r[7].identity=nullptr;break;
  }
  assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 }
 setup();assert(!Record(&device,&list,layers,"{}",{})&&!allocations&&!copies&&!retains);
 setup(4096,2048);layers.boundCb12={};layers.earlyGuides={};
 // Three layers alone are exactly256MiB. The fourth scalar exceeds the cap.
 assert(!run()&&!allocations&&!copies&&!retains&&!workers);
 setup();failRetain=true;assert(!run()&&allocations==8&&retains==1&&!copies&&!workers&&!moduleRefs);
 assert(!GetRegistry(RequestKind::FogLayers).pending&&!GetRegistry(RequestKind::FogLayers).ticket);
 setup();layers.hardwareDepth={};assert(run()&&allocations==7&&copies==7&&!GetRegistry(RequestKind::FogLayers).pending->hardwareDepth);
 setup();GetRegistry(RequestKind::FogLayers).requested=false;
 assert(!run()&&GetRegistry(RequestKind::EarlyGuides).requested&&!allocations&&!copies);
 setup();assert(run()&&GetRegistry(RequestKind::EarlyGuides).requested&&!GetRegistry(RequestKind::EarlyGuides).ticket);
 setup();layers.before=layers.hardwareDepth;layers.after.viewFormat=41;
 assert(!run()&&!allocations&&!copies); // New role must not admit R32 base fog layers.
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-fog-depth-capture-") as directory:
            source, executable = Path(directory) / "capture.cpp", Path(directory) / "capture"
            source.write_text(harness)
            for flags in (("-O0",), ("-O3", "-ffast-math")):
                compiled = subprocess.run([compiler, "-std=c++20", *flags, "-Wall", "-Wextra",
                                           "-I", str(ROOT / "external/nlohmann"), str(source), "-o", str(executable)],
                                          capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                subprocess.run([str(executable)], check=True)

    def test_scalar_raw_writer_still_preserves_nan_signed_zero_and_row_padding(self):
        # Hardware depth uses exactly this existing explicit R32 PrepareEntry and
        # native WriteEntry path. Exercise its actual raw writer, not float math.
        shared.RayCopyCapture().test_compiled_actual_writer_preserves_scalar_and_rgba_bits()


if __name__ == "__main__":
    unittest.main()

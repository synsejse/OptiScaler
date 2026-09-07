"""Bounded raw encoded lighting-input capture, without radiance reinterpretation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.cpp").read_text()
HEADER = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.h").read_text()


def body(name):
    start = SOURCE.index("{", SOURCE.index(name + "("))
    depth, end = 1, start + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class LightingT8Capture(unittest.TestCase):
    def test_additive_optional_api_and_all_checks_precede_allocation(self):
        signature = HEADER.split("bool RecordEarlyGuides", 1)[1].split(";", 1)[0]
        self.assertIn("const Texture* exposureWords = nullptr", signature)
        self.assertIn("const Texture* lightingT8 = nullptr", signature)
        self.assertLess(signature.index("exposureWords"), signature.index("lightingT8"))
        record = body("RecordEarlyGuides")
        self.assertLess(record.index("for (size_t i = 0; i < guides.size(); ++i)"), record.index("if (lightingT8)"))
        for guard in ("!lightingT8->resource", "lightingT8->resource->QueryInterface",
                      "inputIdentity.Get() == identity.Get()", "inputIdentity.Get() == exposureIdentity.Get()",
                      "IsLightingT8Texture(", "PrepareEntry(device, *batch->lightingT8)",
                      "batch->lightingT8->bytes > MaxBytes - totalBytes"):
            self.assertLess(record.index(guard), record.index("AllocateReadback(device, entry)"))
        self.assertIn("totalBytes += batch->lightingT8->bytes", record)
        # Cross-companion alias checks may mention the field, but Fog still has
        # no t8 input or producer/copy admission path.
        self.assertNotIn("lightingT8", HEADER.split("struct Layers", 1)[1].split("};", 1)[0])
        for forbidden in ('batch->lightingT8 =', '.role = "lighting_t8"',
                          'PrepareEntry(device, *batch->lightingT8',
                          'RecordCopy(list, *batch->lightingT8)'):
            self.assertNotIn(forbidden, body("Record"))

    def test_raw_encoded_metadata_preserves_semantic_uncertainty(self):
        record = body("RecordEarlyGuides")
        for field in ('.role = "lighting_t8"', '"optiscaler.fsr_rr.lighting_t8.v1"',
                      'companion["shader_register"] = 8', 'companion["shader_stage"] = "pixel"',
                      '"raw encoded original-lighting input; not established as raw-ray or undenoised radiance"',
                      '"none; native encoded original-lighting input RGBA16F bytes"',
                      '"original binding, ownership, required source read bits and frame association not authenticated',
                      'companion.erase("state_restored")', 'companion["required_read_state"] = 0x8c0',
                      'companion["input_barriers_recorded"] = false',
                      '"engine-managed; required read bits established by caller, exact native state mask not established"'):
            self.assertIn(field, record.replace("caller_supplied; ", ""))
        self.assertEqual(record.count('companion.erase("state_restored")'), 1)
        self.assertLess(record.index('auto companion = Describe(*batch->lightingT8)'),
                        record.index('companion.erase("state_restored")'))
        self.assertIn('entry.filename = std::string(entry.role) + ".rgba16f"', body("PrepareEntry"))
        for forbidden in ("Dispatch(", "SetDescriptorHeaps", "CreateShaderResourceView", "ExposureScale",
                          "float(", "bit_cast<float", "reinterpret_cast<float"):
            self.assertNotIn(forbidden, record)

    def test_immediate_copy_fence_and_worker_do_not_reread_source(self):
        record, worker, copy = body("RecordEarlyGuides"), body("WriteWhenComplete"), body("RecordCopy")
        position = record.index("RecordCopy(list, *batch->lightingT8)")
        self.assertLess(record.index("FSRDSubmission::Retain(device, list, batch)"), position)
        self.assertLess(record.index("batch->keepAlive = keepAlive"), position)
        self.assertLess(position, record.index("recorded = true"))
        self.assertIn("const bool transition = (entry.source.state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0", copy)
        self.assertEqual(copy.count("if (transition)"), 2)
        self.assertLess(worker.index("FSRDSubmission::Complete(args.ticket)"), worker.index("WriteEntry(batch, *batch.lightingT8)"))
        self.assertLess(worker.index("WriteEntry(batch, *batch.lightingT8)"), worker.index('metadata["complete"] = true'))
        write = body("WriteEntry")
        self.assertIn("entry.readback->Map(", write)
        self.assertNotIn("entry.source", write)
        self.assertIn("worker reads only the copied readback", HEADER)
        self.assertIn("no writable alias", HEADER)
        self.assertIn("required read-state tag0x8c0", HEADER)
        self.assertIn("native state may be a read-only superset", HEADER)
        self.assertIn("with NO source barriers", HEADER)

    def test_compiled_actual_exact_typed_layout_predicate(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual lighting-input layout predicate")
        harness = r'''
#include <cassert>
#include <cstdint>
using UINT=uint32_t;using DXGI_FORMAT=uint32_t;using D3D12_RESOURCE_STATES=uint32_t;
constexpr uint32_t MaxDimension=8192,DXGI_FORMAT_R16G16B16A16_FLOAT=10;
constexpr uint32_t D3D12_RESOURCE_DIMENSION_TEXTURE2D=3;
constexpr uint32_t D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=0x40;
constexpr uint32_t D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE=0x80;
constexpr uint32_t D3D12_RESOURCE_STATE_COPY_SOURCE=0x800;
struct D3D12_RESOURCE_DESC {
 uint32_t Dimension=3;uint64_t Width=1280;uint32_t Height=720;
 uint16_t DepthOrArraySize=1,MipLevels=1;uint32_t Format=10;
 struct{uint32_t Count=1,Quality=0;}SampleDesc;
};
bool IsLightingT8Texture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,
 UINT subresource,D3D12_RESOURCE_STATES state,UINT width,UINT height) noexcept
''' + body("IsLightingT8Texture") + r'''
int main() {
 D3D12_RESOURCE_DESC d;assert(IsLightingT8Texture(d,10,0,0x8c0,1280,720));
 for(unsigned field=0;field<10;++field) {
  auto x=d;
  switch(field) {
   case 0:x.Dimension=1;break;case 1:x.Width=1279;break;case 2:x.Height=719;break;
   case 3:x.DepthOrArraySize=2;break;case 4:x.MipLevels=2;break;
   case 5:x.Format=9;break;case 6:x.Format=2;break; // Typeless/FP32 rejected.
   case 7:x.SampleDesc.Count=2;break;case 8:x.SampleDesc.Quality=1;break;
   case 9:x.MipLevels=0;break;
  }
  assert(!IsLightingT8Texture(x,10,0,0x8c0,1280,720));
 }
 assert(!IsLightingT8Texture(d,9,0,0x8c0,1280,720));
 assert(!IsLightingT8Texture(d,10,1,0x8c0,1280,720));
 assert(!IsLightingT8Texture(d,10,0,0x40,1280,720));
 assert(!IsLightingT8Texture(d,10,0,0xc8,1280,720));
 assert(!IsLightingT8Texture(d,10,0,0xc0,1280,720)); // Old shader-read-only tag lacks copy bit.
 assert(!IsLightingT8Texture(d,10,0,0x800,1280,720)); // Copy-only tag lacks required shader reads.
 assert(!IsLightingT8Texture(d,10,0,0x8c8,1280,720)); // Do not accept arbitrary source-state tags.
 assert(!IsLightingT8Texture(d,10,0,0x8e0,1280,720)); // Actual superset is not the required tag.
 assert(!IsLightingT8Texture(d,10,0,0x8c0,0,720));
 assert(!IsLightingT8Texture(d,10,0,0x8c0,1280,0));
 assert(!IsLightingT8Texture(d,10,0,0x8c0,8193,720));
 assert(!IsLightingT8Texture(d,10,0,0x8c0,1280,8193));
 d.Width=8192;d.Height=8192;assert(IsLightingT8Texture(d,10,0,0x8c0,8192,8192));
 // Size acceptance is separate from total readback-budget admission.
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-t8-") as directory:
            path = Path(directory)
            source, binary = path / "layout.cpp", path / "layout"
            source.write_text(harness)
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-O2", str(source),
                                       "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_compiled_actual_copy_records_no_barrier_for_t8(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual native-copy recording")
        harness = r'''
#include <cassert>
#include <cstdint>
#include <utility>
using UINT=uint32_t;using D3D12_RESOURCE_STATES=uint32_t;
constexpr UINT D3D12_RESOURCE_STATE_COPY_SOURCE=0x800;
constexpr UINT D3D12_RESOURCE_BARRIER_TYPE_TRANSITION=0;
constexpr UINT D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=0;
constexpr UINT D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT=1;
struct ID3D12Resource {};
struct ResourcePtr {ID3D12Resource* ptr;ID3D12Resource* Get() const{return ptr;}};
struct Footprint {UINT rowPitch;};
struct Entry {
 struct {ResourcePtr resource;D3D12_RESOURCE_STATES state;UINT subresource;} source;
 ResourcePtr readback;Footprint footprint;
};
struct D3D12_RESOURCE_BARRIER {
 UINT Type;
 struct {ID3D12Resource* pResource;UINT Subresource;D3D12_RESOURCE_STATES StateBefore,StateAfter;}Transition;
};
struct D3D12_TEXTURE_COPY_LOCATION {
 ID3D12Resource* pResource;UINT Type,SubresourceIndex;Footprint PlacedFootprint;
};
struct ID3D12GraphicsCommandList {
 UINT barriers=0,copies=0;D3D12_RESOURCE_BARRIER captured[2]{};
 ID3D12Resource *source=nullptr,*target=nullptr;
 void ResourceBarrier(UINT count,const D3D12_RESOURCE_BARRIER* barrier) {
  assert(count==1&&barriers<2);captured[barriers++]=*barrier;
 }
 void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,
                        const D3D12_TEXTURE_COPY_LOCATION* src,const void* box) {
  assert(!x&&!y&&!z&&!box);
  assert(src->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&src->SubresourceIndex==0);
  assert(dst->Type==D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT&&dst->PlacedFootprint.rowPitch==256);
  ++copies;source=src->pResource;target=dst->pResource;
 }
};
void RecordCopy(ID3D12GraphicsCommandList* list,const Entry& entry)
''' + body("RecordCopy") + r'''
int main() {
 ID3D12Resource original,readback;
 Entry entry{{{&original},0x8c0,0},{&readback},{256}};
 ID3D12GraphicsCommandList list;RecordCopy(&list,entry);
 assert(list.copies==1&&list.barriers==0);
 assert(list.source==&original&&list.target==&readback);
 assert(entry.source.state==0x8c0); // Required mask is only a tag; no StateBefore submitted.
 // Existing private guide/exposure path still transitions its known exact state both ways.
 entry.source.state=0xc0;ID3D12GraphicsCommandList control;RecordCopy(&control,entry);
 assert(control.copies==1&&control.barriers==2);
 assert(control.captured[0].Transition.pResource==&original);
 assert(control.captured[0].Transition.StateBefore==0xc0&&control.captured[0].Transition.StateAfter==0x800);
 assert(control.captured[1].Transition.StateBefore==0x800&&control.captured[1].Transition.StateAfter==0xc0);
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-t8-copy-") as directory:
            path = Path(directory)
            source, binary = path / "copy.cpp", path / "copy"
            source.write_text(harness)
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-O2", str(source),
                                       "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

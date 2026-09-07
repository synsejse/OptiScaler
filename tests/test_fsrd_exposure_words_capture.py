"""Optional native exposure-word readback, without engine access or normalization.

The compiled tests execute the actual production layout/padding predicates. Source
ordering checks complement them; neither stands in for real D3D12 GPU validation.
"""
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


class ExposureWordsCapture(unittest.TestCase):
    def test_optional_standalone_only_native_contract(self):
        signature = HEADER.split("bool RecordEarlyGuides", 1)[1].split(";", 1)[0]
        self.assertIn("const Texture* exposureWords = nullptr", signature)
        record = body("RecordEarlyGuides")
        self.assertLess(record.index("for (size_t i = 0; i < guides.size(); ++i)"),
                        record.index("if (exposureWords)"))
        for guard in ("!exposureWords->resource", "exposureWords->resource->QueryInterface",
                      "exposureIdentity.Get() == identity.Get()",
                      "PrepareEntry(device, *batch->exposureWords, false, false, true)",
                      "batch->exposureWords->bytes > MaxBytes - totalBytes"):
            self.assertLess(record.index(guard), record.index("AllocateReadback(device, entry)"))
        self.assertIn("batch->exposureWords->bytes", record)
        self.assertNotIn("exposureWords", body("Record"))
        prepare = body("PrepareEntry")
        self.assertIn("boundCb12 || guideAlbedo ||", prepare)
        self.assertIn("else if (boundCb12)", prepare)
        self.assertIn("desc.Width != 5", prepare)  # Old cb12 admission stays5x1.
        self.assertIn("CheckSameDevice(device, entry.source.resource.Get())", prepare)

    def test_metadata_does_not_claim_meaning_or_normalize(self):
        record = body("RecordEarlyGuides")
        for field in ('"optiscaler.fsr_rr.exposure_words.v1"',
                      '"source_word_byte_offsets"] = { 0, 4, 8, 12, 16, 20, 24 }',
                      '"source_word_count"] = 7', '"padding_word_index"] = 7',
                      '"padding_word_value"] = 0', '"row-major RGBA uint32 words0..6 then one zero pad"',
                      '"none; raw uint32 bits, no float conversion or exposure normalization"',
                      '"actual GPU-word producer and binding not authenticated'):
            self.assertIn(field, record.replace("caller_supplied; ", ""))
        self.assertIn('.role = "exposure_words"', record)
        for forbidden in ("Dispatch(", "SetPipelineState", "SetDescriptorHeaps", "ReadProcessMemory",
                          "bit_cast<float>", "reinterpret_cast<float", "float("):
            self.assertNotIn(forbidden, record)

    def test_same_retention_fence_and_full_completion(self):
        record = body("RecordEarlyGuides")
        copy = record.index("RecordCopy(list, *batch->exposureWords)")
        self.assertLess(record.index("batch->keepAlive = keepAlive"), copy)
        self.assertLess(record.index("FSRDSubmission::Retain(device, list, batch)"), copy)
        self.assertLess(copy, record.index("recorded = true"))
        worker = body("WriteWhenComplete")
        write = worker.index("WriteEntry(batch, *batch.exposureWords, true)")
        self.assertLess(worker.index("FSRDSubmission::Complete(args.ticket)"), write)
        self.assertLess(write, worker.index('metadata["complete"] = true'))
        entry = body("WriteEntry")
        self.assertLess(entry.index("HasZeroExposurePadding("), entry.index("file.write("))
        self.assertIn("throw std::runtime_error", entry)
        self.assertIn('metadata["complete"] = false', worker)

    def test_compiled_production_layout_and_bitwise_padding_predicates(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual native-word predicates")
        functions = (
            "bool IsExposureWordsTexture(const D3D12_RESOURCE_DESC& desc, DXGI_FORMAT viewFormat,"
            " UINT subresource, D3D12_RESOURCE_STATES state) noexcept" + body("IsExposureWordsTexture") +
            "\nbool HasZeroExposurePadding(const void* pixels, UINT64 rowBytes, UINT rows) noexcept" +
            body("HasZeroExposurePadding"))
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
using UINT=uint32_t; using UINT64=uint64_t; using DXGI_FORMAT=uint32_t;
using D3D12_RESOURCE_STATES=uint32_t;
constexpr uint32_t DXGI_FORMAT_R32G32B32A32_UINT=3;
constexpr uint32_t D3D12_RESOURCE_DIMENSION_TEXTURE2D=3;
constexpr uint32_t D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=0x40;
constexpr uint32_t D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE=0x80;
struct D3D12_RESOURCE_DESC {
 uint32_t Dimension=3; uint64_t Width=2; uint32_t Height=1;
 uint16_t DepthOrArraySize=1,MipLevels=1; uint32_t Format=3;
 struct { uint32_t Count=1,Quality=0; } SampleDesc;
};
''' + functions + r'''
int main() {
 D3D12_RESOURCE_DESC d;
 const auto good=[&]{return IsExposureWordsTexture(d,3,0,0xc0);};
 assert(good());
 for(unsigned field=0;field<9;++field) {
  auto x=d;
  switch(field) {
   case 0: x.Dimension=1;break; case 1: x.Width=5;break; case 2: x.Height=2;break;
   case 3: x.DepthOrArraySize=2;break; case 4: x.MipLevels=2;break;
   case 5: x.Format=2;break; case 6: x.SampleDesc.Count=2;break;
   case 7: x.SampleDesc.Quality=1;break; case 8: x.Width=0;break;
  }
  assert(!IsExposureWordsTexture(x,3,0,0xc0));
 }
 assert(!IsExposureWordsTexture(d,2,0,0xc0));
 assert(!IsExposureWordsTexture(d,3,1,0xc0));
 assert(!IsExposureWordsTexture(d,3,0,0x40));
 assert(!IsExposureWordsTexture(d,3,0,0xc8));
 std::array<uint32_t,8> words={0xffffffff,0x7f800000,0x7fc12345,0x80000000,1,0,0xdeadbeef,0};
 const auto original=words;
 assert(HasZeroExposurePadding(words.data(),32,1));
 assert(words==original); // Native special/sign/NaN bits are not interpreted.
 words[7]=1;assert(!HasZeroExposurePadding(words.data(),32,1));
 words[7]=0x80000000;assert(!HasZeroExposurePadding(words.data(),32,1)); // Not float -0.
 words[7]=0;
 assert(!HasZeroExposurePadding(nullptr,32,1));
 assert(!HasZeroExposurePadding(words.data(),28,1));
 assert(!HasZeroExposurePadding(words.data(),32,2));
 std::array<unsigned char,33> unaligned{};
 std::memcpy(unaligned.data()+1,words.data(),32);
 assert(HasZeroExposurePadding(unaligned.data()+1,32,1));
}
'''
        # Compile actual source predicates without Windows headers or GPU emulation.
        with tempfile.TemporaryDirectory(prefix="fsrd-exposure-words-") as directory:
            path = Path(directory)
            source = path / "words.cpp"
            source.write_text(harness)
            executable = path / "words"
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-O2", str(source),
                                       "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

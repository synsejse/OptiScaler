"""Private original-ray native readbacks: strict roles, ownership, budget and no math."""
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


class RayCopyCapture(unittest.TestCase):
    def test_additive_role_budget_and_identity_guards_precede_allocation(self):
        signature = HEADER.split("bool RecordEarlyGuides", 1)[1].split(";", 1)[0]
        self.assertIn("const std::array<Texture, 2>* rayCopies = nullptr", signature)
        self.assertLess(signature.index("lightingT8"), signature.index("rayCopies"))
        record = body("RecordEarlyGuides")
        for guard in ("!texture.resource", "rayIdentities[i].Get() == identity.Get()",
                      "rayIdentities[i].Get() == exposureIdentity.Get()",
                      "rayIdentities[i].Get() == inputIdentity.Get()",
                      "rayIdentities[i].Get() == rayIdentities[previous].Get()",
                      "IsRayCopyTexture(", "PrepareEntry(device, entry, false, false, false, i == 1)",
                      "guides and optional ray companions exceed the shared256MiB readback limit"):
            self.assertLess(record.index(guard), record.index("AllocateReadback(device, entry)"))
        self.assertNotIn("rayCopies", HEADER.split("struct Layers", 1)[1].split("};", 1)[0])
        for forbidden in ('batch->rayCopies =', '.role = "ray_motion"', '.role = "ray_hit"',
                          'for (auto& entry : *batch->rayCopies) AllocateReadback',
                          'for (const auto& entry : *batch->rayCopies)\n            RecordCopy'):
            self.assertNotIn(forbidden, body("Record"))
        self.assertIn("std::optional<std::array<Entry, 2>> rayCopies", SOURCE)
        self.assertIn("bool exposureWords = false, bool rayHit = false", SOURCE)
        prepare = body("PrepareEntry")
        self.assertIn("if (rayHit)", prepare)
        self.assertIn("boundCb12 || guideAlbedo || exposureWords", prepare)
        self.assertIn('entry.channels = "R"', prepare)
        self.assertIn('entry.filename = std::string(entry.role) + ".r32f"', prepare)
        self.assertIn('{ "channels", entry.channels }', body("Describe"))

    def test_retention_worker_and_provenance_are_explicit(self):
        record = body("RecordEarlyGuides")
        self.assertLess(record.index("FSRDSubmission::Retain(device, list, batch)"), record.index("if (!args->ticket)"))
        self.assertLess(record.index("if (!args->ticket)"), record.index("RecordCopy(list, entry)"))
        self.assertLess(record.index("batch->keepAlive = keepAlive"), record.index("RecordCopy(list, entry)"))
        for field in ('"optiscaler.fsr_rr.private_ray_copy.v1"', '"original_ray_dispatch; caller_supplied"',
                      '"none; native byte copy, all channels and bits preserved"',
                      '"caller must prove motion scale and hit encoding; not inferred from format"',
                      '"caller must prove selected writer and any final-write claim; not established by readback completion"'):
            self.assertIn(field, record)
        writer = body("WriteWhenComplete")
        copies = writer.index("if (batch.rayCopies)")
        self.assertLess(writer.index("FSRDSubmission::Complete(args.ticket)"), copies)
        self.assertLess(copies, writer.index('metadata["complete"] = true'))
        self.assertIn("for (const auto& entry : *batch.rayCopies)\n                WriteEntry(batch, entry)", writer)
        self.assertIn("entry.readback->Map(", body("WriteEntry"))
        self.assertNotIn("entry.source", body("WriteEntry"))
        for forbidden in ("Dispatch(", "CreateShaderResourceView", "bit_cast<float", "reinterpret_cast<float"):
            self.assertNotIn(forbidden, record)

    def test_compiled_actual_prepare_and_record_contracts(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile actual capture functions")
        structs = SOURCE[SOURCE.index("struct Entry"):SOURCE.index("enum class RequestKind")]
        functions = "\n".join(signature + body(name) for name, signature in (
            ("IsExposureWordsTexture", "bool IsExposureWordsTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state) noexcept"),
            ("IsLightingT8Texture", "bool IsLightingT8Texture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state,UINT width,UINT height) noexcept"),
            ("IsRayCopyTexture", "bool IsRayCopyTexture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT viewFormat,UINT subresource,D3D12_RESOURCE_STATES state,UINT width,UINT height,bool rayHit) noexcept"),
            ("PrepareEntry", "void PrepareEntry(ID3D12Device* device,Entry& entry,bool boundCb12=false,bool guideAlbedo=false,bool exposureWords=false,bool rayHit=false)"),
            ("Describe", "Json Describe(const Entry& entry)")))
        harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <json.hpp>
using Json=nlohmann::json;
using UINT=uint32_t;using UINT64=uint64_t;using DXGI_FORMAT=uint32_t;
using D3D12_RESOURCE_STATES=uint32_t;using HMODULE=void*;using LPCWSTR=const wchar_t*;
constexpr UINT64 MaxBytes=256ull*1024*1024;
constexpr UINT MaxDimension=8192,MaxProvenanceBytes=256*1024;
constexpr UINT DXGI_FORMAT_R8G8B8A8_UNORM=28,DXGI_FORMAT_R16G16B16A16_FLOAT=10;
constexpr UINT DXGI_FORMAT_R16G16B16A16_TYPELESS=9,DXGI_FORMAT_R32G32B32A32_FLOAT=2;
constexpr UINT DXGI_FORMAT_R32G32B32A32_TYPELESS=1,DXGI_FORMAT_R32G32B32A32_UINT=3;
constexpr UINT DXGI_FORMAT_R32_FLOAT=41,D3D12_RESOURCE_DIMENSION_TEXTURE2D=3;
constexpr UINT D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=0x40;
constexpr UINT D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE=0x80;
constexpr UINT D3D12_RESOURCE_STATE_COPY_SOURCE=0x800,D3D12_COMMAND_LIST_TYPE_DIRECT=0;
constexpr UINT GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS=4;
struct IUnknown {};
template<class T> struct ComPtr {
 T* ptr=nullptr;ComPtr()=default;ComPtr(T* p):ptr(p){}
 T* Get() const{return ptr;}T* operator->() const{return ptr;}T** operator&(){return &ptr;}
 explicit operator bool()const{return ptr!=nullptr;}
};
#define IID_PPV_ARGS(p) p
struct D3D12_RESOURCE_DESC {
 UINT Dimension=3;UINT64 Width=1280;UINT Height=720;
 uint16_t DepthOrArraySize=1,MipLevels=1;UINT Format=10;
 struct{UINT Count=1,Quality=0;}SampleDesc;
};
struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
 UINT64 Offset=0;struct{UINT Width=0,Height=0,Depth=1,RowPitch=0;}Footprint;
};
struct ID3D12Resource: IUnknown {
 D3D12_RESOURCE_DESC desc;IUnknown* identity=this;bool failIdentity=false;
 auto GetDesc() const{return desc;}
 int QueryInterface(IUnknown** out){*out=failIdentity?nullptr:identity;return failIdentity?-1:0;}
};
struct ID3D12GraphicsCommandList {UINT type=0;UINT GetType(){return type;}};
struct ID3D12Device {
 void GetCopyableFootprints(const D3D12_RESOURCE_DESC* d,UINT sub,UINT count,UINT64 offset,
                           D3D12_PLACED_SUBRESOURCE_FOOTPRINT* f,UINT* rows,UINT64* rowBytes,UINT64* bytes){
  assert(!sub&&count==1&&!offset);
  UINT pixel=d->Format==28||d->Format==41?4:d->Format==10||d->Format==9?8:16;
  f->Footprint.Width=UINT(d->Width);f->Footprint.Height=d->Height;
  *rows=d->Height;*rowBytes=d->Width*pixel;
  f->Footprint.RowPitch=UINT((*rowBytes+255)&~UINT64(255));
  *bytes=UINT64(f->Footprint.RowPitch)*(*rows-1)+*rowBytes;
 }
};
struct Texture {ComPtr<ID3D12Resource> resource;UINT subresource=0;DXGI_FORMAT viewFormat=0;D3D12_RESOURCE_STATES state=0;};
unsigned allocations=0,copies=0,retains=0,workers=0,moduleRefs=0;
bool failRetain=false;
void Check(int value,const char* text){if(value<0)throw std::runtime_error(text);}
template<class T>void CheckSameDevice(ID3D12Device*,T*){}
''' + structs + functions + r'''
enum class RequestKind{FogLayers,EarlyGuides};
struct Status{bool queued=false,attempted=false;std::string message,directory;};
namespace FSRDSubmission {
 struct Ticket{};
 std::shared_ptr<Ticket> Retain(ID3D12Device*,ID3D12GraphicsCommandList*,const std::shared_ptr<Batch>& batch){
  ++retains;assert(batch&&batch->keepAlive);return failRetain?nullptr:std::make_shared<Ticket>();
 }
}
struct Registry {
 std::mutex mutex;
 std::atomic<bool> requested{true};Status status;
 std::shared_ptr<Batch> pending;std::shared_ptr<FSRDSubmission::Ticket> ticket;
};
Registry& GetRegistry(RequestKind kind=RequestKind::EarlyGuides){
 static Registry registries[2];return registries[kind==RequestKind::FogLayers?0:1];
}
void AllocateReadback(ID3D12Device*,Entry&){++allocations;}
void RecordCopy(ID3D12GraphicsCommandList*,const Entry& entry){
 assert(GetRegistry().ticket);assert(entry.source.state==0xc0||entry.source.state==0x8c0);++copies;
}
namespace Util {std::filesystem::path ExePath(){return "/not-written/game.exe";}}
std::string Timestamp(const char*){return "test";}
UINT GetCurrentProcessId(){return 1;}
void FinishStatus(RequestKind,bool,const std::string&,bool){}
struct WorkerArgs{HMODULE module=nullptr;std::shared_ptr<Batch> batch;std::shared_ptr<FSRDSubmission::Ticket> ticket;};
bool GetModuleHandleExW(UINT,LPCWSTR,HMODULE* m){*m=reinterpret_cast<void*>(1);++moduleRefs;return true;}
void FreeLibrary(HMODULE){assert(moduleRefs);--moduleRefs;}
void WriterThread(void*){}
void* CreateThread(void*,UINT,void(*)(void*),void* raw,UINT,void*){
 ++workers;auto* args=static_cast<WorkerArgs*>(raw);assert(args->ticket);FreeLibrary(args->module);delete args;
 return reinterpret_cast<void*>(1);
}
void CloseHandle(void*){}
bool RecordEarlyGuides(ID3D12Device* device,ID3D12GraphicsCommandList* list,
 const std::array<Texture,3>& guides,const std::string& provenanceJson,
 const std::shared_ptr<void>& keepAlive,const Texture* exposureWords=nullptr,
 const Texture* lightingT8=nullptr,const std::array<Texture,2>* rayCopies=nullptr) noexcept
''' + body("RecordEarlyGuides") + r'''
int main(){
 ID3D12Device device;ID3D12GraphicsCommandList list;
 ID3D12Resource resources[7];
 std::array<Texture,3> guides;std::array<Texture,2> ray;
 Texture exposure,t8;auto owner=std::make_shared<int>(1);
 auto setup=[&](UINT width=1280,UINT height=720){
  allocations=copies=retains=workers=moduleRefs=0;failRetain=false;
  auto& registry=GetRegistry();registry.requested=true;
  GetRegistry(RequestKind::FogLayers).requested=false;
  registry.pending.reset();registry.ticket.reset();
  for(unsigned i=0;i<7;++i){resources[i].desc={};resources[i].desc.Width=width;
   resources[i].desc.Height=height;resources[i].identity=&resources[i];resources[i].failIdentity=false;}
  for(unsigned i=0;i<3;++i){resources[i].desc.Format=i<2?28:10;guides[i]={&resources[i],0,resources[i].desc.Format,0xc0};}
  resources[3].desc.Format=10;resources[4].desc.Format=41;
  ray={Texture{&resources[3],0,10,0xc0},Texture{&resources[4],0,41,0xc0}};
  resources[5].desc.Width=2;resources[5].desc.Height=1;resources[5].desc.Format=3;
  exposure={&resources[5],0,3,0xc0};t8={&resources[6],0,10,0x8c0};
 };
 auto run=[&](bool withExposure=true,bool withT8=true){return RecordEarlyGuides(&device,&list,guides,"{}",owner,
  withExposure?&exposure:nullptr,withT8?&t8:nullptr,&ray);};
 setup();assert(run());assert(allocations==7&&copies==7&&retains==1&&workers==1&&!moduleRefs);
 auto b=GetRegistry().pending;assert(b&&b->rayCopies);
 const auto& motion=(*b->rayCopies)[0];const auto& hit=(*b->rayCopies)[1];
 assert(motion.filename=="ray_motion.rgba16f"&&hit.filename=="ray_hit.r32f");
 assert(Describe(motion)["channels"]=="RGBA"&&Describe(hit)["channels"]=="R");
 assert(Describe(hit)["component_type"]=="float32"&&Describe(hit)["pixel_bytes"]==4);
 assert(Describe(hit)["row_bytes"]==1280*4&&Describe(hit)["file_bytes"]==1280*720*4);
 assert(b->metadata["companions"].size()==7&&b->metadata["readback_bytes"]>0);
 assert(b->metadata["companions"][6]["source_role"]=="hit");
 assert(!run()&&copies==7); // Existing one-shot request cannot be consumed twice.
 for(unsigned source=0;source<2;++source){
  for(unsigned target=0;target<7;++target){
   if(target==source+3)continue;
   setup();resources[source+3].identity=&resources[target];
   assert(!run()&&!allocations&&!copies&&!retains&&!workers);
  }
  for(unsigned field=0;field<14;++field){
   setup();auto& t=ray[source];auto& d=resources[source+3].desc;
   switch(field){
    case 0:t.resource=nullptr;break;case 1:t.subresource=1;break;case 2:t.viewFormat=1;break;
    case 3:d.Format=source?39:9;break;case 4:d.Dimension=1;break;case 5:d.MipLevels=2;break;
    case 6:d.DepthOrArraySize=2;break;case 7:d.SampleDesc.Count=2;break;
    case 8:d.SampleDesc.Quality=1;break;case 9:t.state=0x8c0;break;
    case 10:d.Width=1279;break;case 11:d.Height=719;break;
    case 12:resources[source+3].failIdentity=true;break;case 13:t.state=0x40;break;
   }
   assert(!run()&&!allocations&&!copies&&!retains&&!workers);
  }
 }
 setup(4096,2048); // Guides128MiB+t8 64+motion64+hit32 exceeds256MiB at hit.
 assert(!run(false,true)&&!allocations&&!copies&&!retains&&!workers);
 setup();failRetain=true;assert(!run()&&allocations==7&&retains==1&&!copies&&!workers&&!moduleRefs);
 assert(!GetRegistry().pending&&!GetRegistry().ticket);
 setup();assert(RecordEarlyGuides(&device,&list,guides,"{}",owner));
 assert(allocations==3&&copies==3&&workers==1&&!GetRegistry().pending->rayCopies);
 setup();GetRegistry().requested=false;GetRegistry(RequestKind::FogLayers).requested=true;
 assert(!run()&&GetRegistry(RequestKind::FogLayers).requested&&!copies&&!allocations);
 setup();Entry e{.role="forbidden_base_hit",.source=ray[1]};bool refused=false;
 try{PrepareEntry(&device,e);}catch(...){refused=true;}assert(refused);
 for(unsigned conflict=0;conflict<3;++conflict){
  refused=false;try{PrepareEntry(&device,e,conflict==0,conflict==1,conflict==2,true);}catch(...){refused=true;}
  assert(refused);
 }
 PrepareEntry(&device,e,false,false,false,true);assert(e.pixelBytes==4&&e.filename=="forbidden_base_hit.r32f");
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-ray-capture-") as directory:
            path = Path(directory)
            source, executable = path / "capture.cpp", path / "capture"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", optimization,
                                           "-I", str(ROOT / "external/nlohmann"), str(source), "-o", str(executable)],
                                          capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                subprocess.run([str(executable)], check=True, timeout=30)

    def test_compiled_actual_writer_preserves_scalar_and_rgba_bits(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile native byte writer")
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
using UINT=uint32_t;using UINT64=uint64_t;using SIZE_T=size_t;
struct D3D12_RANGE{SIZE_T Begin,End;};
struct ID3D12Resource{
 std::array<unsigned char,1024> memory{};unsigned maps=0,unmaps=0;
 int Map(UINT sub,const D3D12_RANGE* range,void** out){
  assert(!sub&&!range->Begin&&range->End<=memory.size());++maps;*out=memory.data();return 0;
 }
 void Unmap(UINT sub,const D3D12_RANGE* range){assert(!sub&&!range->Begin&&!range->End);++unmaps;}
};
struct Pointer{ID3D12Resource* p;ID3D12Resource* Get()const{return p;}ID3D12Resource* operator->()const{return p;}};
struct Entry{
 Pointer readback;std::string filename;UINT64 bytes=0,rowBytes=0;UINT rows=0;
 struct{UINT64 Offset=0;struct{UINT RowPitch=0;}Footprint;}footprint;
};
struct Batch{std::filesystem::path directory;};
void Check(int result,const char* message){if(result)throw std::runtime_error(message);}
bool HasZeroExposurePadding(const void* pixels,UINT64 rowBytes,UINT rows) noexcept
''' + body("HasZeroExposurePadding") + r'''
void WriteEntry(const Batch& batch,const Entry& entry,bool exposureWords=false)
''' + body("WriteEntry") + r'''
int main(int argc,char** argv){
 assert(argc==2);Batch batch{argv[1]};ID3D12Resource readback;
 const std::array<uint32_t,8> words={0x80000000,0x7f800000,0x7fc12345,0x00000001,
                                  0xff800000,0x7fa00001,0x3f800000,0xffffffff};
 // Typed R32F retains all bit patterns, including NaN payloads; no float loads.
 for(unsigned role=0;role<2;++role){
  readback.memory.fill(0xcc);
  Entry e{{&readback},role?"ray_motion.rgba16f":"ray_hit.r32f",1024,role?16u:8u,2,{16,{256}}};
  std::memcpy(readback.memory.data()+16,words.data(),size_t(e.rowBytes));
  std::memcpy(readback.memory.data()+16+256,reinterpret_cast<const char*>(words.data())+e.rowBytes,size_t(e.rowBytes));
  WriteEntry(batch,e);
  std::ifstream file(batch.directory/e.filename,std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
  assert(bytes.size()==2*e.rowBytes&&!std::memcmp(bytes.data(),words.data(),bytes.size()));
  assert(!std::filesystem::exists(batch.directory/(e.filename+".part")));
 }
 assert(readback.maps==2&&readback.unmaps==2);
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-ray-native-write-") as directory:
            path = Path(directory)
            source, executable = path / "writer.cpp", path / "writer"
            source.write_text(harness)
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-O3", "-ffast-math",
                                       str(source), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(executable), directory], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

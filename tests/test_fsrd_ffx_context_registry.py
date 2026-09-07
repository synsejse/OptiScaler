"""Compile actual FFX routing helpers/call paths with concurrent distinct contexts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/proxies/FfxApi_Proxy.h").read_text()


def function(name):
    start = SOURCE.index(name + "(")
    opening = SOURCE.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[SOURCE.rfind("\n", 0, start) + 1:end]


class FfxContextRegistry(unittest.TestCase):
    def test_all_map_access_is_inside_small_locked_helpers(self):
        self.assertIn("#include <mutex>", SOURCE)
        self.assertIn("inline static std::mutex contextToTypeMutex", SOURCE)
        register, lookup = function("RememberContextType"), function("FindContextType")
        for helper in (register, lookup):
            self.assertIn("std::lock_guard lock(contextToTypeMutex)", helper)
            for forbidden in ("LOG_", "CreateContext(", "DestroyContext(", "Configure(", "Query(", "Dispatch("):
                self.assertNotIn(forbidden, helper)
        rest = SOURCE.replace(register, "").replace(lookup, "")
        self.assertEqual(rest.count("contextToType"), 2)  # map + mutex declarations only
        create = function("D3D12_CreateContext")
        self.assertEqual(create.count("RememberContextType(*context, type)"), 4)
        for module in ("fg_dx12", "upscaling_dx12", "denoiser_dx12", "radiance_dx12"):
            call = create.index(module + ".CreateContext(context, desc, memCb)")
            remember = create.index("RememberContextType(*context, type)", call)
            self.assertLess(call, remember)
        destroy = function("D3D12_DestroyContext")
        self.assertIn("FindContextType(*context, FFXStructType::Unknown, true)", destroy)
        self.assertLess(destroy.index("FindContextType("), destroy.index(".DestroyContext("))
        self.assertIn("FindContextType(*context, FFXStructType::General)", function("D3D12_Configure"))
        for name in ("D3D12_CreateContext", "D3D12_DestroyContext", "D3D12_Configure"):
            self.assertNotIn("lock_guard", function(name))

    def test_compiled_actual_routing_concurrency_and_provider_reentry(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile FFX context routing tests")
        methods = "\n".join(function(name) for name in (
            "RememberContextType", "FindContextType", "D3D12_CreateContext",
            "D3D12_DestroyContext", "D3D12_Configure"))
        harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
using ffxContext=void*;using ffxReturnCode_t=int;
constexpr int FFX_API_RETURN_OK=0,FFX_API_RETURN_ERROR=-1,FFX_API_RETURN_NO_PROVIDER=-2;
enum class FFXStructType{General,Upscaling,FG,SwapchainDX12,SwapchainVulkan,Denoiser,RadianceCache,Unknown};
struct ffxCreateContextDescHeader{FFXStructType type;};
struct ffxConfigureDescHeader{FFXStructType type;};
struct ffxAllocationCallbacks{};
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
// std::lock_guard in the ACTUAL helper drives a real mutex, instrumented only
// to assert that no provider callback/logging scope inherits the registry lock.
thread_local unsigned registryLocks=0;
struct CheckedMutex{
 std::mutex mutex;
 void lock(){mutex.lock();assert(!registryLocks);++registryLocks;}
 void unlock(){assert(registryLocks==1);--registryLocks;mutex.unlock();}
};
struct Module{
 void* dll=nullptr;
 bool skipCreateCalls=false,skipConfigureCalls=false,skipQueryCalls=false;
 int(*CreateContext)(ffxContext*,ffxCreateContextDescHeader*,const ffxAllocationCallbacks*)=nullptr;
 int(*DestroyContext)(ffxContext*,const ffxAllocationCallbacks*)=nullptr;
 int(*Configure)(ffxContext*,const ffxConfigureDescHeader*)=nullptr;
};
class Proxy{
public:
 inline static std::unordered_map<ffxContext,FFXStructType> contextToType;
 inline static CheckedMutex contextToTypeMutex;
 inline static Module main_dx12,fg_dx12,upscaling_dx12,denoiser_dx12,radiance_dx12;
 inline static bool _skipDestroyCalls=false;
 static FFXStructType GetType(FFXStructType type){return type;}
''' + methods + r'''
};
std::atomic<uintptr_t> serial{1};
std::atomic<unsigned> creates{0},destroys{0},configures{0};
thread_local bool reentry=false;
void Reenter(){
 assert(!registryLocks);
 if(reentry)return;
 reentry=true;
 ffxContext inner=nullptr;ffxCreateContextDescHeader create{FFXStructType::RadianceCache};
 assert(Proxy::D3D12_CreateContext(&inner,&create,nullptr)==FFX_API_RETURN_OK);
 ffxConfigureDescHeader config{FFXStructType::General};
 assert(Proxy::D3D12_Configure(&inner,&config)==FFX_API_RETURN_OK);
 assert(Proxy::D3D12_DestroyContext(&inner,nullptr)==FFX_API_RETURN_OK&&!inner);
 reentry=false;
}
template<FFXStructType Kind> int Create(ffxContext* context,ffxCreateContextDescHeader* desc,const ffxAllocationCallbacks*){
 assert(!registryLocks&&desc->type==Kind);Reenter();
 *context=reinterpret_cast<void*>((serial.fetch_add(1)<<4)|uintptr_t(Kind));
 assert(Proxy::FindContextType(*context,FFXStructType::Unknown)==FFXStructType::Unknown);
 ++creates;return FFX_API_RETURN_OK;
}
template<FFXStructType Kind> int Destroy(ffxContext* context,const ffxAllocationCallbacks*){
 assert(!registryLocks);
 if((uintptr_t(*context)&15)!=uintptr_t(Kind))return FFX_API_RETURN_ERROR;
 // Lookup/take is complete before invoking the provider, even during reentry.
 assert(Proxy::FindContextType(*context,FFXStructType::Unknown)==FFXStructType::Unknown);
 Reenter();*context=nullptr;++destroys;return FFX_API_RETURN_OK;
}
template<FFXStructType Kind> int Configure(ffxContext* context,const ffxConfigureDescHeader*){
 assert(!registryLocks&&(uintptr_t(*context)&15)==uintptr_t(Kind));
 assert(Proxy::FindContextType(*context,FFXStructType::Unknown)==Kind);
 Reenter();++configures;return FFX_API_RETURN_OK;
}
template<FFXStructType Kind>Module Provider(){
 return {reinterpret_cast<void*>(1),false,false,false,Create<Kind>,Destroy<Kind>,Configure<Kind>};
}
int main(){
 Proxy::fg_dx12=Provider<FFXStructType::FG>();Proxy::upscaling_dx12=Provider<FFXStructType::Upscaling>();
 Proxy::denoiser_dx12=Provider<FFXStructType::Denoiser>();Proxy::radiance_dx12=Provider<FFXStructType::RadianceCache>();
 constexpr std::array kinds{FFXStructType::FG,FFXStructType::Upscaling,FFXStructType::Denoiser,FFXStructType::RadianceCache};
 std::vector<std::thread> threads;
 for(unsigned thread=0;thread<8;++thread)threads.emplace_back([thread,kinds]{
  for(unsigned i=0;i<500;++i){
   auto kind=kinds[thread%4];ffxContext context=nullptr;ffxCreateContextDescHeader create{kind};
   assert(Proxy::D3D12_CreateContext(&context,&create,nullptr)==FFX_API_RETURN_OK);
   ffxConfigureDescHeader config{FFXStructType::General};
   assert(Proxy::D3D12_Configure(&context,&config)==FFX_API_RETURN_OK);
   assert(Proxy::FindContextType(context,FFXStructType::Unknown)==kind);
   assert(Proxy::D3D12_DestroyContext(&context,nullptr)==FFX_API_RETURN_OK&&!context);
  }
 });
 for(auto& thread:threads)thread.join();
 assert(creates==16000&&configures==16000&&destroys==16000);
 assert(Proxy::contextToType.empty()); // Only after all threads joined.
 // Missing lookups never insert; General remains General when no mapping exists.
 ffxContext missing=reinterpret_cast<void*>(0x987);
 ffxConfigureDescHeader general{FFXStructType::General};
 assert(Proxy::D3D12_Configure(&missing,&general)==FFX_API_RETURN_NO_PROVIDER);
 assert(Proxy::FindContextType(missing,FFXStructType::Unknown,true)==FFXStructType::Unknown);
 assert(Proxy::contextToType.empty());
 // Copy the returned type, never an iterator/reference whose lifetime crosses unlock.
 Proxy::RememberContextType(missing,FFXStructType::Denoiser);
 auto copied=Proxy::FindContextType(missing,FFXStructType::Unknown,true);
 assert(copied==FFXStructType::Denoiser&&Proxy::contextToType.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-ffx-registry-") as directory:
            path = Path(directory)
            source, executable = path / "registry.cpp", path / "registry"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                                           "-pthread", optimization, str(source), "-o", str(executable)],
                                          capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

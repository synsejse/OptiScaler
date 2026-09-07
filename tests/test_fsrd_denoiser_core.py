"""Provider-only extraction: actual C++ core/denoiser header, mocked common API and calls.

This is a lifecycle/dispatch/settings test, not a replacement for the Windows ABI build.
The base FFX submodule is optional locally, so common API PODs are mocked in a temp dir.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "OptiScaler/upscalers/ffx/FSRDDenoiserCore.h"
FEATURE = ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp"


class DenoiserCore(unittest.TestCase):
    def test_no_feature_or_render_policy_in_provider_core(self):
        source = CORE.read_text()
        for forbidden in ("NVSDK_", "Config::", "State::", "Handle()", "SetDescriptorHeaps",
                          "ResourceBarrier", "FSRDSubmission", "changeBackend", "GetRenderResolution"):
            self.assertNotIn(forbidden, source)
        self.assertIn("const ffxDispatchDescDenoiser& description", source)
        self.assertIn("no pointer into that possibly stack-owned chain is stored", source)
        self.assertIn("does NOT add fences or change the existing teardown policy", source)

    def test_feature_preserves_policy_and_effective_settings(self):
        source = FEATURE.read_text()
        self.assertNotIn("&_pDenoiserCtx", source)
        self.assertIn("_denoiser.Create(_denoiserCtxDesc)", source)
        self.assertIn("_denoiser.QueryDefaults()", source)
        self.assertIn("_denoiser.Configure(requested)", source)
        self.assertIn("_denoiser.Dispatch(dispatchDesc)", source)
        self.assertIn("_denoiser.AbandonOnProcessShutdown()", source)
        create = source.split("bool FSRDFeatureDx12::CreateDenoiserContext()", 1)[1].split(
            "bool FSRDFeatureDx12::CreateNativeDebugResources()", 1)[0]
        self.assertLess(create.index("ScopedSkipHeapCapture"), create.index("_denoiser.Create("))
        self.assertIn("ScopedSkipSpoofingGlobal", create)
        self.assertIn("if (!QueryDefaultDenoiserSettings())", create)
        self.assertIn("DestroyDenoiserContext();", create)
        self.assertIn("state.changeBackend[Handle()->Id] = true", source)
        self.assertIn("const auto& settings = _denoiser.Settings();", source)

    def test_compiled_production_core_recording_provider(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to a host C++20 compiler for actual provider-core tests")
        api = r'''
#pragma once
#include <cstdint>
using ffxContext = void*;
using ffxApiMessage = void (*)(uint32_t, const wchar_t*);
enum ffxReturnCode_t { FFX_API_RETURN_OK, FFX_API_RETURN_ERROR_PARAMETER,
                      FFX_API_RETURN_ERROR_RUNTIME_ERROR };
struct ffxHeader { uint64_t type{}; ffxHeader* pNext{}; };
using ffxCreateContextDescHeader = ffxHeader;
using ffxConfigureDescHeader = ffxHeader;
using ffxQueryDescHeader = ffxHeader;
using ffxDispatchDescHeader = ffxHeader;
struct ffxAllocationCallbacks {};
#define FFX_API_EFFECT_MASK 0xffff0000u
#define FFX_API_BACKEND_MASK 0xff000000u
'''
        types = r'''
#pragma once
#include <cstdint>
struct FfxApiDimensions2D { uint32_t width{}, height{}; };
struct FfxApiFloatCoords2D { float x{}, y{}; };
struct FfxApiResource { void* resource{}; uint32_t state{}; };
struct FfxApiEffectMemoryUsage {};
'''
        harness = r'''
#include <cassert>
#include <cstring>
#include <type_traits>
#include <vector>
#include "FSRDDenoiserCore.h"

struct Mock
{
    static inline unsigned creates=0, destroys=0, dispatches=0;
    static inline uint64_t queryFail=0, configureFail=0;
    static inline bool createFail=false;
    static inline ffxReturnCode_t dispatchResult=FFX_API_RETURN_OK;
    static inline ffxCreateContextDescHeader* createdDescription=nullptr;
    static inline const ffxDispatchDescHeader* dispatchedDescription=nullptr;
    static inline std::vector<uint64_t> queries, configured;
    static inline std::vector<float> values;
    static ffxReturnCode_t D3D12_CreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                               const ffxAllocationCallbacks* callbacks)
    {
        assert(!*context && callbacks==nullptr);
        createdDescription=desc; ++creates;
        if(createFail) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        *context=reinterpret_cast<void*>(uintptr_t(creates));
        return FFX_API_RETURN_OK;
    }
    static ffxReturnCode_t D3D12_DestroyContext(ffxContext* context, const ffxAllocationCallbacks* callbacks)
    {
        assert(*context && callbacks==nullptr); ++destroys;
        // Deliberately leave the pointer set: core must not destroy twice.
        return FFX_API_RETURN_OK;
    }
    static ffxReturnCode_t D3D12_Query(ffxContext* context, ffxQueryDescHeader* header)
    {
        assert(*context);
        auto& desc=*reinterpret_cast<ffxQueryDescDenoiserGetDefaultKeyValue*>(header);
        assert(desc.header.type==FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE && desc.count==1);
        queries.push_back(desc.key);
        if(desc.key==queryFail) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        *static_cast<float*>(desc.data)=float(desc.key)*10;
        return FFX_API_RETURN_OK;
    }
    static ffxReturnCode_t D3D12_Configure(ffxContext* context, const ffxConfigureDescHeader* header)
    {
        assert(*context);
        const auto& desc=*reinterpret_cast<const ffxConfigureDescDenoiserKeyValue*>(header);
        assert(desc.header.type==FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE && desc.count==1);
        configured.push_back(desc.key); values.push_back(*static_cast<const float*>(desc.data));
        return desc.key==configureFail ? FFX_API_RETURN_ERROR_RUNTIME_ERROR : FFX_API_RETURN_OK;
    }
    static ffxReturnCode_t D3D12_Dispatch(ffxContext* context, const ffxDispatchDescHeader* header)
    {
        assert(*context); ++dispatches; dispatchedDescription=header; return dispatchResult;
    }
};
using Core=FSRD::DenoiserCore<Mock>;
static_assert(!std::is_copy_constructible_v<Core> && !std::is_move_constructible_v<Core>);

int main()
{
    ffxCreateContextDescDenoiser create{};
    create.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    ffxCreateContextDescHeader backend{}; create.header.pNext=&backend;
    ffxDispatchDescDenoiser dispatch{};
    dispatch.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    ffxDispatchDescDenoiserInput1Signal signal{};
    dispatch.header.pNext=&signal.header;
    dispatch.commandList=reinterpret_cast<void*>(0x1234);
    dispatch.linearDepth.resource=reinterpret_cast<void*>(0x2345);
    dispatch.flags=FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO|FFX_DENOISER_DISPATCH_RESET;
    dispatch.frameIndex=543; dispatch.deltaTime=17.25f;
    const auto original=dispatch;
    {
        Core core;
        assert(!core.IsCreated());
        assert(core.QueryDefaults().code==FFX_API_RETURN_ERROR_PARAMETER);
        assert(core.Configure({}).code==FFX_API_RETURN_ERROR_PARAMETER);
        assert(core.Dispatch(dispatch)==FFX_API_RETURN_ERROR_PARAMETER);
        assert(Mock::queries.empty() && Mock::configured.empty() && Mock::dispatches==0);
        core.Destroy(); assert(Mock::destroys==0);
        assert(core.Create(create)==FFX_API_RETURN_OK && core.IsCreated());
        assert(!core.HasSettings());
        assert(core.Configure({}).code==FFX_API_RETURN_ERROR_PARAMETER && Mock::configured.empty());
        assert(Mock::createdDescription==&create.header && create.header.pNext==&backend);
        assert(core.Create(create)==FFX_API_RETURN_ERROR_PARAMETER && Mock::creates==1);
        assert(core.QueryDefaults().code==FFX_API_RETURN_OK);
        assert(core.HasSettings());
        assert(Mock::queries==std::vector<uint64_t>({1,2,3,4,5,6}));
        assert(core.Settings().crossBilateralNormalStrength==10 && core.Settings().stabilityBias==20);
        assert(core.Settings().maxRadiance==30 && core.Settings().radianceClipStdK==40);
        assert(core.Settings().gaussianKernelRelaxation==50 && core.Settings().disocclusionThreshold==60);
        assert(core.Configure(core.Settings()).code==FFX_API_RETURN_OK && Mock::configured.empty());
        auto request=core.Settings();
        request.crossBilateralNormalStrength=11; request.stabilityBias=21; request.maxRadiance=31;
        Mock::configureFail=2;
        auto failed=core.Configure(request);
        assert(failed.code==FFX_API_RETURN_ERROR_RUNTIME_ERROR && failed.key==2);
        assert(Mock::configured==std::vector<uint64_t>({1,2}));
        assert(core.Settings().crossBilateralNormalStrength==11 && core.Settings().stabilityBias==20);
        assert(core.Settings().maxRadiance==30);
        Mock::configureFail=0; Mock::configured.clear();
        assert(core.Configure(request).code==FFX_API_RETURN_OK);
        assert(Mock::configured==std::vector<uint64_t>({2,3})); // Don't repeat successful key1.
        assert(core.QueryDefaults().code==FFX_API_RETURN_ERROR_PARAMETER); // Defaults aren't current values.
        assert(core.Settings().stabilityBias==21 && Mock::queries.size()==6);
        assert(core.Dispatch(dispatch)==FFX_API_RETURN_OK);
        assert(Mock::dispatchedDescription==&dispatch.header && dispatch.header.pNext==&signal.header);
        assert(std::memcmp(&dispatch,&original,sizeof(dispatch))==0);
        Mock::dispatchResult=FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        assert(core.Dispatch(dispatch)==FFX_API_RETURN_ERROR_RUNTIME_ERROR && Mock::dispatches==2);
        assert(core.IsCreated()); // No inferred reset/recreate/recovery policy.
        core.Destroy(); core.Destroy(); assert(Mock::destroys==1 && !core.IsCreated() && !core.HasSettings());
    }
    assert(Mock::destroys==1);
    {
        Core core; assert(core.Create(create)==FFX_API_RETURN_OK);
        Mock::queries.clear(); Mock::queryFail=3;
        const auto failed=core.QueryDefaults();
        assert(failed.code==FFX_API_RETURN_ERROR_RUNTIME_ERROR && failed.key==3);
        assert(Mock::queries==std::vector<uint64_t>({1,2,3}));
    }
    assert(Mock::destroys==2); // Also owns cleanup after defaults query failure.
    {
        Core core; Mock::createFail=true;
        assert(core.Create(create)==FFX_API_RETURN_ERROR_RUNTIME_ERROR && !core.IsCreated());
    }
    assert(Mock::destroys==2);
    {
        Core core; Mock::createFail=false; assert(core.Create(create)==FFX_API_RETURN_OK);
        core.AbandonOnProcessShutdown(); assert(!core.IsCreated());
    }
    assert(Mock::destroys==2); // Existing process-shutdown provider unload exception.
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-core-test-") as name:
            directory = Path(name)
            (directory / "ffx_api.h").write_text(api)
            (directory / "ffx_api_types.h").write_text(types)
            source = directory / "core_test.cpp"
            source.write_text(harness)
            executable = directory / "core_test"
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-pedantic", str(source),
                                       "-I", str(CORE.parent), "-I", str(ROOT / "OptiScaler/include"),
                                       "-I", str(directory), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

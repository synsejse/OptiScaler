#pragma once
#include "Config.h"
#include "State.h"
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_defs_dlssd.h>

namespace FSRD
{
constexpr bool IsReadableInputState(int32_t state)
{
    constexpr uint32_t readableStates =
        D3D12_RESOURCE_STATE_GENERIC_READ | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    const auto bits = static_cast<uint32_t>(state);
    // COMMON permits normal read promotion. Combined shader/depth read states are also valid;
    // do not accept an illegal mixture that happens to include the NON_PIXEL bit alongside a write.
    return state == D3D12_RESOURCE_STATE_COMMON ||
           ((bits & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0 && (bits & ~readableStates) == 0);
}

static_assert(IsReadableInputState(D3D12_RESOURCE_STATE_COMMON));
static_assert(IsReadableInputState(D3D12_RESOURCE_STATE_GENERIC_READ));
static_assert(IsReadableInputState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ));
static_assert(!IsReadableInputState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
static_assert(!IsReadableInputState(D3D12_RESOURCE_STATE_RENDER_TARGET));
static_assert(!IsReadableInputState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
static_assert(!IsReadableInputState(-1));

constexpr bool IsSupportedMotionLayout(bool jittered, bool lowResolution, uint32_t renderWidth, uint32_t renderHeight,
                                       uint32_t displayWidth, uint32_t displayHeight)
{
    return !jittered && (lowResolution || (renderWidth == displayWidth && renderHeight == displayHeight));
}

static_assert(IsSupportedMotionLayout(false, true, 1280, 720, 2560, 1440));
static_assert(IsSupportedMotionLayout(false, false, 2560, 1440, 2560, 1440));
static_assert(!IsSupportedMotionLayout(true, true, 1280, 720, 2560, 1440));
static_assert(!IsSupportedMotionLayout(false, false, 1280, 720, 2560, 1440));
static_assert(!IsSupportedMotionLayout(false, false, 2560, 720, 2560, 1440));

constexpr bool HasUnambiguousInputView(DXGI_FORMAT format, bool hardwareDepth)
{
    // The resource alone cannot distinguish FP16 from UNORM for this format family.
    // R16_TYPELESS depth remains supported through its D16/R16_UNORM interpretation;
    // applying that global view to a floating-point hit distance or roughness would be wrong.
    return hardwareDepth || format != DXGI_FORMAT_R16_TYPELESS;
}

static_assert(!HasUnambiguousInputView(DXGI_FORMAT_R16_TYPELESS, false));
static_assert(HasUnambiguousInputView(DXGI_FORMAT_R16_TYPELESS, true));
static_assert(HasUnambiguousInputView(DXGI_FORMAT_R16_FLOAT, false));
static_assert(HasUnambiguousInputView(DXGI_FORMAT_R16_UNORM, false));
static_assert(HasUnambiguousInputView(DXGI_FORMAT_R32_FLOAT, false));

inline bool ValidateInputContract(const NVSDK_NGX_Parameter& parameters, const Config& config, const State& state,
                                  bool hardwareDepth, bool packedRoughness)
{
    // Reject ambiguous R16_TYPELESS for consumed color, motion, normal, roughness, albedo,
    // hit-distance and linear-depth inputs. Only raw hardware depth has a defined UNORM view.
    // Do not inspect an unused separate roughness input when normal alpha supplies roughness.
    struct InputBinding
    {
        const char* key;
        bool consumed;
        bool hardwareDepth;
    };
    const InputBinding inputs[] = { { NVSDK_NGX_Parameter_Color, true, false },
                                    { NVSDK_NGX_Parameter_MotionVectors, true, false },
                                    { NVSDK_NGX_Parameter_Depth, true, hardwareDepth },
                                    { NVSDK_NGX_Parameter_GBuffer_Normals, true, false },
                                    { NVSDK_NGX_Parameter_GBuffer_Roughness, !packedRoughness, false },
                                    { NVSDK_NGX_Parameter_DiffuseAlbedo, true, false },
                                    { NVSDK_NGX_Parameter_SpecularAlbedo, true, false },
                                    { NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, true, false } };
    for (const auto& input : inputs)
    {
        if (!input.consumed)
            continue;
        const char* key = input.key;
        ID3D12Resource* resource = nullptr;
        NVSDK_NGX_Result result = parameters.Get(key, &resource);
        if (result != NVSDK_NGX_Result_Success)
            result = parameters.Get(key, reinterpret_cast<void**>(&resource));
        if (result == NVSDK_NGX_Result_Success && resource &&
            !HasUnambiguousInputView(resource->GetDesc().Format, input.hardwareDepth))
        {
            LOG_ERROR("FSR-RR cannot infer the numeric format of R16_TYPELESS input {}; only hardware depth has a "
                      "defined UNORM view. Provide a typed resource",
                      key);
            return false;
        }
    }

    const auto CheckState = [](const auto& option, const char* name)
    {
        if (option.has_value() && !IsReadableInputState(option.value()))
        {
            LOG_ERROR("FSR-RR requires shader-readable NGX inputs before conversion; unsupported {} state {}", name,
                      option.value());
            return false;
        }
        return true;
    };
    if (!CheckState(config.ColorResourceBarrier, "color") || !CheckState(config.MVResourceBarrier, "motion") ||
        !CheckState(config.DepthResourceBarrier, "depth"))
        return false;

    const bool unrealAutoBarriers = state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                                    state.gameEngine == GameEngineType::Unreal ||
                                    state.gameQuirks & GameQuirk::ForceUnrealEngine;
    if (unrealAutoBarriers && (!config.ColorResourceBarrier.has_value() || !config.MVResourceBarrier.has_value()))
    {
        LOG_ERROR("FSR-RR does not support Unreal's automatic late input barriers; provide readable NGX inputs");
        return false;
    }

    // Conversion, SR, and debug blits currently consume origin-zero rectangles. Do not silently
    // denoise or write a different rectangle than the one the engine supplied.
    const char* offsets[] = { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                              NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X,
                              NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y,
                              NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
                              NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
                              NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                              NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y };
    for (const auto* key : offsets)
    {
        unsigned int value = 0;
        if (parameters.Get(key, &value) == NVSDK_NGX_Result_Success && value != 0)
        {
            LOG_ERROR("FSR-RR does not support nonzero NGX subrect origins: {}={}", key, value);
            return false;
        }
    }
    return true;
}
} // namespace FSRD

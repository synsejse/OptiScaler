#pragma once

#include "fsr-rr/ffx_denoiser.h"

namespace FSRD
{
struct DenoiserSettings
{
    float crossBilateralNormalStrength {};
    float stabilityBias {};
    float maxRadiance {};
    float radianceClipStdK {};
    float gaussianKernelRelaxation {};
    float disocclusionThreshold {};
};

struct DenoiserSettingResult
{
    ffxReturnCode_t code = FFX_API_RETURN_OK;
    uint64_t key = 0; // Zero for an invalid context/settings lifecycle; otherwise the failed provider key.
};

// RR provider only: no NGX, Config, camera history, resource conversion or SR policy.
// Api is the existing FfxApiProxy in production and a recording provider in CPU tests.
// Calls borrow descriptors/resources synchronously for command recording. The caller must
// serialize a context's use and retain it until its GPU work is complete before Destroy or
// destruction. This extraction does NOT add fences or change the existing teardown policy.
template <class Api> class DenoiserCore
{
  public:
    DenoiserCore() = default;
    DenoiserCore(const DenoiserCore&) = delete;
    DenoiserCore& operator=(const DenoiserCore&) = delete;
    DenoiserCore(DenoiserCore&&) = delete;
    DenoiserCore& operator=(DenoiserCore&&) = delete;
    ~DenoiserCore() { Destroy(); }

    // The complete backend/version chain is supplied by the caller and consumed here;
    // no pointer into that possibly stack-owned chain is stored by this core.
    ffxReturnCode_t Create(ffxCreateContextDescDenoiser& description)
    {
        if (_context)
            return FFX_API_RETURN_ERROR_PARAMETER;
        _settings = {};
        _settingsKnown = false;
        return Api::D3D12_CreateContext(&_context, &description.header, nullptr);
    }

    void Destroy()
    {
        if (_context)
        {
            Api::D3D12_DestroyContext(&_context, nullptr);
            _context = nullptr;
        }
        _settingsKnown = false;
    }

    // Preserve the feature's pre-existing process-shutdown exception: intentionally do
    // not call a provider that may already be unloading. Not a runtime retirement path.
    void AbandonOnProcessShutdown()
    {
        _context = nullptr;
        _settingsKnown = false;
    }

    bool IsCreated() const { return _context != nullptr; }
    bool HasSettings() const { return _settingsKnown; }
    // Valid only after successful QueryDefaults; thereafter tracks actual configured values.
    const DenoiserSettings& Settings() const { return _settings; }

    DenoiserSettingResult QueryDefaults()
    {
        // Defaults are not a query of CURRENT settings. Never overwrite effective values
        // with provider defaults once initialization/configuration has completed.
        if (!_context || _settingsKnown)
            return { FFX_API_RETURN_ERROR_PARAMETER, 0 };
        for (const auto& entry : Entries)
        {
            ffxQueryDescDenoiserGetDefaultKeyValue description = {
                .header = { .type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE },
                .key = static_cast<uint64_t>(entry.key), .count = 1, .data = &(_settings.*entry.member)
            };
            const auto result = Api::D3D12_Query(&_context, &description.header);
            if (result != FFX_API_RETURN_OK)
                return { result, description.key };
        }
        _settingsKnown = true;
        return {};
    }

    DenoiserSettingResult Configure(const DenoiserSettings& requested)
    {
        // Without defaults a requested zero could incorrectly match a zero-initialized cache.
        if (!_context || !_settingsKnown)
            return { FFX_API_RETURN_ERROR_PARAMETER, 0 };
        for (const auto& entry : Entries)
        {
            float& current = _settings.*entry.member;
            const float& value = requested.*entry.member;
            if (current == value)
                continue;
            ffxConfigureDescDenoiserKeyValue description = {
                .header = { .type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE },
                .key = static_cast<uint64_t>(entry.key), .count = 1, .data = &value
            };
            const auto result = Api::D3D12_Configure(&_context, &description.header);
            if (result != FFX_API_RETURN_OK)
                return { result, description.key };
            // Earlier successful keys remain effective if a later configure call fails.
            current = value;
        }
        return {};
    }

    ffxReturnCode_t Dispatch(const ffxDispatchDescDenoiser& description)
    {
        if (!_context)
            return FFX_API_RETURN_ERROR_PARAMETER;
        // Forward the exact same descriptor/extension chain; do not infer reset, camera,
        // signal resources or ownership from a feature/global previous frame.
        return Api::D3D12_Dispatch(&_context, &description.header);
    }

  private:
    struct Entry
    {
        FfxApiConfigureDenoiserKey key;
        float DenoiserSettings::*member;
    };
    static constexpr Entry Entries[] = {
        { FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH,
          &DenoiserSettings::crossBilateralNormalStrength },
        { FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS, &DenoiserSettings::stabilityBias },
        { FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE, &DenoiserSettings::maxRadiance },
        { FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K, &DenoiserSettings::radianceClipStdK },
        { FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION,
          &DenoiserSettings::gaussianKernelRelaxation },
        { FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD, &DenoiserSettings::disocclusionThreshold }
    };
    ffxContext _context = nullptr;
    DenoiserSettings _settings {};
    bool _settingsKnown = false;
};
} // namespace FSRD

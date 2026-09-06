#pragma once
#include "FFXFeature_Dx12.h"
#include "shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h"
#include "fsr-rr/ffx_denoiser.h"
#include <DirectXMath.h>

/**
 * @brief Unified denoiser-upscaler utilising AMD FSR Ray Regeneration and Super Resolution with
 * DLSS-RR inputs. Extends the FFX (FSR 3.1/4) upscaler implementation.
 */
class FSRDFeatureDx12 : public FFXFeatureDx12
{
  public:
    using FSRDConvDesc = FSRDPreprocessor_Dx12::ConversionDesc;

    FSRDFeatureDx12(uint32_t InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~FSRDFeatureDx12();

    feature_version Version() override { return FFXFeatureDx12::Version(); }

    Upscaler GetUpscalerType() const override { return Upscaler::FSRD; }

    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    // The generic backend-recreation path only copies SR parameters. Preserve the RR input layout too.
    void CopyRRCreateParameters(NVSDK_NGX_Parameter* parameters) const;

  private:
    struct DenoiserSettings
    {
        float crossBilateralNormalStrength {};
        float stabilityBias {};
        float maxRadiance {};
        float radianceClipStdK {};
        float gaussianKernelRelaxation {};
        float disocclusionThreshold {};
    };

    ffxContext _pDenoiserCtx;
    ffxCreateContextDescDenoiser _denoiserCtxDesc;
    DenoiserSettings _denoiserSettings;
    bool _isMode2;
    bool _isInReset;
    uint32_t _captureSamples = 0;

    bool _isHWDepth = false;
    bool _isRoughnessPacked = false;
    bool _hasCameraHistory = false;
    uint32_t _lastRenderWidth = 0;
    uint32_t _lastRenderHeight = 0;

    FSRDConvDesc _convDesc;
    DirectX::XMFLOAT3 _lastCamPos; // Last world space camera position

    // Matrices
    DirectX::XMMATRIX _invViewMatrix;  // Camera rotation and translation
    DirectX::XMMATRIX _viewMatrix;     // World to camera space
    DirectX::XMMATRIX _prevViewMatrix; // Last world to camera space
    DirectX::XMMATRIX _projMatrix;     // Perspective projection matrix

    // Upscaler dispatch overrides, applied via OverrideUpscaleDispatch()
    ID3D12Resource* _upscaleColorOverride;
    float _upscaleFovVertical;
    float _upscaleDeltaTime;

    std::unique_ptr<FSRDPreprocessor_Dx12> FSRDConvShader;

    bool InitFFX(const NVSDK_NGX_Parameter* InParameters) override;

    void OverrideUpscaleDispatch(ffxDispatchDescUpscale& params) override;

    bool CreateDenoiserContext();

    bool QueryDenoiserVersions();

    bool QueryDefaultDenoiserSettings();

    void DestroyDenoiserContext();

    bool UpdateSize(const NVSDK_NGX_Parameter* parameters);

    void CaptureInputs(const NVSDK_NGX_Parameter& inParams, const ffxDispatchDescDenoiser& dispatchDesc);

    /**
     * @brief Generates FSR denoiser configuration and input buffers from DLSS-RR inputs and NGX configurations,
     * converts and repacks resources internally.
     */
    template <typename SignalDescT>
    bool PrepareDenoiserInput(ID3D12GraphicsCommandList* InCommandList, const NVSDK_NGX_Parameter& ngxParams,
                              ffxDispatchDescDenoiser& dispatchDesc, SignalDescT& signalDesc);

    /**
     * @brief Retrieves DLSS-RR inputs to populate the inputs for the conversion shader in order to generate
     FSR-RR compatible buffers.
     */
    bool PrepareDenoiseConvInput(const NVSDK_NGX_Parameter& inParams);

    /**
     * @brief Converts previously retrieved DLSS-RR resources into FSR-RR inputs.
     */
    bool ConvertDenoiserBuffers(ID3D12GraphicsCommandList* InCommandList);

    /**
     * @brief Dispatches FSR-RR denoiser converted inputs. Runs before upscaler.
     */
    bool DispatchDenoiser(ID3D12GraphicsCommandList* InCommandList, const ffxDispatchDescDenoiser& dispatchDesc);
};

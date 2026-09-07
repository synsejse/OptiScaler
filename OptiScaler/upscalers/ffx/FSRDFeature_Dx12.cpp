#include "pch.h"
#include <nvsdk_ngx_defs_dlssd.h>
#include <DirectXMath.h>
#include "NVNGX_Parameter.h"
#include "FSRDFeature_Dx12.h"
#include "FSRDDiagnosticPolicy.h"
#include "shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h"
#include "shaders/fsrd_preprocess/FSRDShaderUtils.h"
#include "MathUtils.h"
#include "FSRDInputMath.h"
#include "FSRDInputValidation.h"
#include "FSRDResearchCapture.h"
#include "FSRDCyberpunkFogProbe.h"
#include <json.hpp>

using namespace DirectX;
using namespace OptiMath;

using FSRDConvDesc = FSRDPreprocessor_Dx12::ConversionDesc;
using FSRDCompDesc = FSRDPreprocessor_Dx12::CompositionDesc;

/**
 * @brief Retrieves a matrix from the given parameter table. Matrices used by DLSS are in column-major
 * order, but DirectXMath operations assume row-major. Appropriate for passing to DirectX shaders, but not for
 * CPU-side operations without transposing.
 */
static bool TryGetNGXMatrixTranspose(const NVSDK_NGX_Parameter& ngxParams, const char* key, DirectX::XMMATRIX& outValue)
{
    float* pMat = nullptr;

    if (ngxParams.Get(key, (void**) &pMat) == NVSDK_NGX_Result_Success && pMat != nullptr)
    {
        memcpy_s(&outValue, sizeof(DirectX::XMMATRIX), pMat, sizeof(float) * 16);
        return true;
    }
    else
        return false;
}

/**
 * @brief Retrieves a matrix from the given parameter table and transposes it for CPU-side
 * operations with DirectXMath.
 */
static bool TryGetNGXMatrix(const NVSDK_NGX_Parameter& ngxParams, const char* key, DirectX::XMMATRIX& outValue)
{
    if (TryGetNGXMatrixTranspose(ngxParams, key, outValue))
    {
        outValue = XMMatrixTranspose(outValue);
        return true;
    }
    else
        return false;
}

static bool IsUsableMatrix(const XMMATRIX& matrix)
{
    XMFLOAT4X4 values;
    XMStoreFloat4x4(&values, matrix);
    for (const auto& row : values.m)
        for (float value : row)
            if (!std::isfinite(value) || std::abs(value) >= 1e30f) // Includes Streamline's INVALID_FLOAT sentinel.
                return false;
    const float determinant = XMVectorGetX(XMMatrixDeterminant(matrix));
    return std::isfinite(determinant) && std::abs(determinant) > 1e-20f;
}

// The authenticated Cyberpunk encoding can be linearized without projected XY only for
// conventional perspective depth. Reject oblique/depth-offset projections rather than guess.
static bool HasSeparablePerspectiveDepth(const XMMATRIX& matrix)
{
    return matrix.r[2].m128_f32[0] == 0.0f && matrix.r[2].m128_f32[1] == 0.0f &&
           matrix.r[3].m128_f32[0] == 0.0f && matrix.r[3].m128_f32[1] == 0.0f &&
           matrix.r[3].m128_f32[3] == 0.0f && std::abs(matrix.r[3].m128_f32[2]) == 1.0f &&
           std::isfinite(matrix.r[2].m128_f32[2]) && std::isfinite(matrix.r[2].m128_f32[3]) &&
           matrix.r[2].m128_f32[3] != 0.0f;
}

template <typename T>
static bool TryGetLoggedResource(const NVSDK_NGX_Parameter& ngxParams, const char* key, T*& outValue)
{
    const bool success = TryGetNGXVoidPointer(ngxParams, key, outValue);

    if (success)
        LOG_DEBUG("{} exists..", key);
    else
        LOG_ERROR("{} is missing!!", key);

    return success;
}

/**
 * @brief Calculates vertical FOV according to: FOVv = 2 * arctan( 1 / M22 )
 * @param proj View to Clip / Perspective projection matrix
 * @return Vertical field of view in radians
 */
static float GetVertFovFromProjectionMatrixRad(const XMMATRIX& proj)
{
    return float(2.0 * (std::atan(1.0 / (double) proj.r[1].m128_f32[1])));
}

/**
 * @brief Calculates horizontal FOV according to: FOVh = 2 * arctan( 1 / M11 )
 * @param proj View to Clip / Perspective projection matrix
 * @return Horizontal field of view in radians
 */
static float GetHorzFovFromProjectionMatrixRad(const XMMATRIX& proj)
{
    return float(2.0 * (std::atan(1.0 / (double) proj.r[0].m128_f32[0])));
}

/**
 * @brief Calculates aspect ratio (width / height) as AR = M22 / M11
 * @param proj View to Clip / Perspective projection matrix
 * @return Aspect ratio as an fp32 decimal e.g. 1.778
 */
static float GetAspectRatioFromProjectionMatrix(const XMMATRIX& proj)
{
    return proj.r[1].m128_f32[1] / proj.r[0].m128_f32[0];
}

static XMFLOAT3 GetFloat3(const XMVECTOR& vec4)
{
    XMFLOAT3 vec3 = {};
    XMStoreFloat3(&vec3, vec4);
    return vec3;
}

static XMVECTOR GetColumn(const XMMATRIX& mat, int col)
{
    return { mat.r[0].m128_f32[col], mat.r[1].m128_f32[col], mat.r[2].m128_f32[col], 0 };
}

static void SetColumn(const XMVECTOR& vec, int col, XMMATRIX& mat)
{
    mat.r[0].m128_f32[col] = vec.m128_f32[0];
    mat.r[1].m128_f32[col] = vec.m128_f32[1];
    mat.r[2].m128_f32[col] = vec.m128_f32[2];
    mat.r[3].m128_f32[col] = vec.m128_f32[3];
}

static XMFLOAT3 GetFloat3Column(const XMMATRIX& mat, int col)
{
    return { mat.r[0].m128_f32[col], mat.r[1].m128_f32[col], mat.r[2].m128_f32[col] };
}

static FfxApiFloatCoords3D GetFloat3ColumnFFX(const XMMATRIX& mat, int col)
{
    return { mat.r[0].m128_f32[col], mat.r[1].m128_f32[col], mat.r[2].m128_f32[col] };
}

static FfxApiFloatCoords3D GetFloat3FFX(const XMVECTOR& vec4)
{
    FfxApiFloatCoords3D vec3 = {};
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&vec3), vec4);
    return vec3;
}

static const FfxApiFloatCoords3D& GetFloat3FFX(const XMFLOAT3& vec3)
{
    return *reinterpret_cast<const FfxApiFloatCoords3D*>(&vec3);
}

static ID3D12Resource* GetD3D12ResFromFFX(const FfxApiResource& resource)
{
    return static_cast<ID3D12Resource*>(resource.resource);
}

using FSRD::ViewPlanes;

static ViewPlanes GetViewPlanes(const DirectX::XMMATRIX& projection, bool isInverted)
{
    // View to clip
    float A = projection.r[2].m128_f32[2];
    float B = projection.r[2].m128_f32[3];
    float W = projection.r[3].m128_f32[2];

    return FSRD::GetViewPlanes(A, B, W, isInverted);
}

using FSRDConvFlags = FSRDPreprocessor_Dx12::ConvFlags;
using FSRDCompFlags = FSRDPreprocessor_Dx12::CompFlags;

enum class DebugModes : uint64_t
{
    None = 0,
    DenoiserBypass = 1,
    UpscalerBypass = 2,
    RawColor = 3,
    DlssBias = 4,
    DlssColorBeforeParticles = 5,
    DlssColorBeforeTransparency = 6,
    DlssTransparencyLayer = 7,

    ConversionDebug = FSRDConvFlags::Debug,
    ConversionDebugMask = FSRDConvFlags::DebugModeMask,

    OutRadiance = FSRDConvFlags::DebugOutRadiance,

    InSpecHitDist = FSRDConvFlags::DebugInSpecHitDist,
    InDepth = FSRDConvFlags::DebugInDepth,
    InMotion = FSRDConvFlags::DebugInMotion,
    InNormals = FSRDConvFlags::DebugInNormals,
    InRoughness = FSRDConvFlags::DebugInRoughness,
    InDiffAlbedo = FSRDConvFlags::DebugInDiffAlbedo,
    InSpecAlbedo = FSRDConvFlags::DebugInSpecAlbedo,

    OutFusedAlbedo = FSRDConvFlags::DebugOutFusedAlbedo,
    OutLinearDepth = FSRDConvFlags::DebugOutLinearDepth,
    OutMotion = FSRDConvFlags::DebugOutMotion,
    OutNormals = FSRDConvFlags::DebugOutNormals,
    OutSpecAlbedo = FSRDConvFlags::DebugOutSpecAlbedo,
    OutDiffAlbedo = FSRDConvFlags::DebugOutDiffAlbedo,

    OutDepthDelta = FSRDConvFlags::DebugOutDepthDelta,
    OutNormDotView = FSRDConvFlags::DebugOutNormDotView,
    AlbedoError = FSRDConvFlags::DebugAlbedoError,


    CompositionDebugOffset = 16u,
    CompositionDebug = (uint64_t) FSRDCompFlags::Debug << CompositionDebugOffset,
    CompositionDebugMask = (uint64_t) FSRDCompFlags::DebugModeMask,

    SkipSignal = (uint64_t) FSRDCompFlags::DebugSkipSignal << CompositionDebugOffset,
    DenoiserOutput = (uint64_t) FSRDCompFlags::DebugDenoiserOutput << CompositionDebugOffset,
    FusedLighting = (uint64_t) FSRDCompFlags::DebugFusedLighting << CompositionDebugOffset,
};

static FSRDConvFlags GetConvDebugFlags(DebugModes mode)
{
    uint32_t flags = uint32_t(mode);
    flags &= uint32_t(DebugModes::ConversionDebugMask);
    return FSRDConvFlags(flags);
}

static FSRDCompFlags GetCompDebugFlags(DebugModes mode)
{
    uint64_t flags = uint64_t(mode);
    flags >>= uint64_t(DebugModes::CompositionDebugOffset);
    flags &= uint64_t(DebugModes::CompositionDebugMask);
    return FSRDCompFlags(flags);
}

using ModeNamePair = std::pair<const char*, uint64_t>;
constexpr auto kDebugModes = std::to_array<ModeNamePair>({
    { "None", (uint64_t) DebugModes::None },

    { "DenoiserBypass", (uint64_t) DebugModes::DenoiserBypass },
    { "UpscalerBypass", (uint64_t) DebugModes::UpscalerBypass },
    { "DenoiserOutput", (uint64_t) DebugModes::DenoiserOutput },
    { "SkipSignal", (uint64_t) DebugModes::SkipSignal },

    { "RawColor", (uint64_t) DebugModes::RawColor },
    { "DlssBias", (uint64_t) DebugModes::DlssBias },
    { "DlssColorBeforeParticles", (uint64_t) DebugModes::DlssColorBeforeParticles },
    { "DlssColorBeforeTransparency", (uint64_t) DebugModes::DlssColorBeforeTransparency },
    { "DlssTransparencyLayer", (uint64_t) DebugModes::DlssTransparencyLayer },

    { "InDepth", (uint64_t) DebugModes::InDepth },
    { "InMotionVectors", (uint64_t) DebugModes::InMotion },
    { "InNormals", (uint64_t) DebugModes::InNormals },
    { "InRoughness", (uint64_t) DebugModes::InRoughness },
    { "InSpecHitDist", (uint64_t) DebugModes::InSpecHitDist },
    { "InDiffAlbedo", (uint64_t) DebugModes::InDiffAlbedo },
    { "InSpecAlbedo", (uint64_t) DebugModes::InSpecAlbedo },

    { "OutRadiance", (uint64_t) DebugModes::OutRadiance },
    { "OutFusedAlbedo", (uint64_t) DebugModes::OutFusedAlbedo },
    { "OutLinearDepth", (uint64_t) DebugModes::OutLinearDepth },
    { "OutMotionVectors", (uint64_t) DebugModes::OutMotion },
    { "OutNormals", (uint64_t) DebugModes::OutNormals },
    { "OutSpecAlbedo", (uint64_t) DebugModes::OutSpecAlbedo },
    { "OutDiffAlbedo", (uint64_t) DebugModes::OutDiffAlbedo },
    { "OutDepthDelta", (uint64_t) DebugModes::OutDepthDelta },
    { "OutNormDotView", (uint64_t) DebugModes::OutNormDotView },

    { "AlbedoError", (uint64_t) DebugModes::AlbedoError },


    { "FusedLighting", (uint64_t) DebugModes::FusedLighting },
});

FSRDFeatureDx12::FSRDFeatureDx12(uint32_t InHandleId, NVSDK_NGX_Parameter* InParameters)
    : FFXFeatureDx12(InHandleId, InParameters), IFeature(InHandleId, SetParameters(InParameters)),
      _denoiserCtxDesc({}), _convDesc({}), _isInReset(false),
      _lastCamPos(0.0f, 0.0f, 0.0f), _invViewMatrix(XMMatrixIdentity()), _viewMatrix(XMMatrixIdentity()),
      _prevViewMatrix(XMMatrixIdentity()), _projMatrix(XMMatrixIdentity()), _prevProjMatrix(XMMatrixIdentity()),
      _upscaleColorOverride(nullptr),
      _upscaleFovVertical(0.0f), _upscaleDeltaTime(0.0f)
{
    _lastDenoiserFrameTime = Util::MillisecondsNow();
    _moduleLoaded = FfxApiProxy::IsDenoiserReady();

    if (_moduleLoaded)
        LOG_INFO("amd_fidelityfx_denoiser_dx12.dll methods loaded!");
    else
        LOG_ERROR("can't load amd_fidelityfx_denoiser_dx12.dll methods!");
}

FSRDFeatureDx12::~FSRDFeatureDx12()
{
    if (State::Instance().isShuttingDown)
    {
        _denoiser.AbandonOnProcessShutdown();
        return;
    }

    DestroyDenoiserContext();
}

bool FSRDFeatureDx12::InitFFX(const NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    // Init upscaler first - borrow some init boilerplate and some cfg
    if (FFXFeatureDx12::InitFFX(InParameters))
    {
        SetInit(false);

        LOG_DEBUG("FSR Ray Regeneration Initializing");
        _name = OptiTexts::FSR_RR_Name;

        if (int value; InParameters->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &value) == NVSDK_NGX_Result_Success)
            _isHWDepth = value == NVSDK_NGX_DLSS_Depth_Type_HW;

        if (int value; InParameters->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &value) == NVSDK_NGX_Result_Success)
            _isRoughnessPacked = value == NVSDK_NGX_DLSS_Roughness_Mode_Packed;

        LOG_INFO("DLSSD Flags HWDepth: {} - IsRoughnessPacked: {}", _isHWDepth, _isRoughnessPacked);

        if (!CreateDenoiserContext())
            return false;

        LOG_INFO("FSR Ray Regeneration Initialized");

        SetInit(true);
        return true;
    }

    return false;
}

void FSRDFeatureDx12::CopyRRCreateParameters(NVSDK_NGX_Parameter* parameters) const
{
    parameters->Set(NVSDK_NGX_Parameter_Use_HW_Depth,
                    static_cast<int>(_isHWDepth ? NVSDK_NGX_DLSS_Depth_Type_HW : NVSDK_NGX_DLSS_Depth_Type_Linear));
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode,
                    static_cast<int>(_isRoughnessPacked ? NVSDK_NGX_DLSS_Roughness_Mode_Packed
                                                       : NVSDK_NGX_DLSS_Roughness_Mode_Unpacked));
}

bool FSRDFeatureDx12::CreateDenoiserContext()
{
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    if (!QueryDenoiserVersions())
        return false;

    state.ffxDenoiserUpscalerVersion = FFXFeature::Version();
    _denoiserVersion.parse_version(state.ffxDenoiserVersionNames[cfg.FfxDenoiserIndex.value_or_default()]);
    _denoiserProviderId = state.ffxDenoiserVersionIds[cfg.FfxDenoiserIndex.value_or_default()];
    _denoiserProviderName = state.ffxDenoiserVersionNames[cfg.FfxDenoiserIndex.value_or_default()];

    ffxOverrideVersion vidOverride = { .header = { .type = FFX_API_DESC_TYPE_OVERRIDE_VERSION },
                                       .versionId =
                                           state.ffxDenoiserVersionIds[cfg.FfxDenoiserIndex.value_or_default()] };
    // Create context
    // Backend desc
    ffxCreateBackendDX12Desc backendDesc = { .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
                                                         .pNext = &vidOverride.header }, // Chain override into backend
                                             .device = Device };
    // Chain: ContextDesc -> BackendDesc -> OverrideVersion
    // Composited radiance with fused albedo without a dominant light source
    _denoiserCtxDesc = { .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER,
                                     // Chain backend desc into context desc
                                     .pNext = &backendDesc.header },
                         .version = FFX_DENOISER_VERSION,
                         // Reserve once. A quality change resets history, never frees in-flight RR resources.
                         .maxRenderSize = { std::max(RenderWidth(), DisplayWidth()),
                                            std::max(RenderHeight(), DisplayHeight()) },
                         .mode = FFX_DENOISER_MODE_1_SIGNAL,
                         .flags = 0 };

    // Opt in at context creation only. The GUI never destroys a live GPU context.
    if (cfg.FfxDenoiserNativeDebug.value_or_default())
        _denoiserCtxDesc.flags |= FFX_DENOISER_ENABLE_DEBUGGING;

    // Create the denoiser context
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        auto ret = _denoiser.Create(_denoiserCtxDesc);

        if (ret != FFX_API_RETURN_OK)
        {
            LOG_ERROR("_denoiserCtx error: {0}", FfxApiProxy::ReturnCodeToString(ret));
            return false;
        }
    }

    if (!QueryDefaultDenoiserSettings())
    {
        DestroyDenoiserContext();
        return false;
    }

    // Keep "auto" tied to the selected RR 1.1 provider rather than pinning RR 1.0 defaults.
    const auto& settings = _denoiser.Settings();
    const auto ApplyProviderDefault = [](CustomOptional<float>& option, float value)
    {
        if (!option.value_for_config().has_value())
            option.set_volatile_value(value);
    };

    ApplyProviderDefault(cfg.FfxDenoiserCrossBlNormStr, settings.crossBilateralNormalStrength);
    ApplyProviderDefault(cfg.FfxDenoiserStabilityBias, settings.stabilityBias);
    ApplyProviderDefault(cfg.FfxDenoiserMaxRadiance, settings.maxRadiance);
    ApplyProviderDefault(cfg.FfxDenoiserRadianceClip, settings.radianceClipStdK);
    ApplyProviderDefault(cfg.FfxDenoiserGaussKernRelax, settings.gaussianKernelRelaxation);
    ApplyProviderDefault(cfg.FfxDenoiserDisocclusionThreshold, settings.disocclusionThreshold);

    // Create DLSS-RR to FSR-RR input converter
    FSRDConvShader = std::make_unique<FSRDPreprocessor_Dx12>("FSRD Converter", Device);

    if (!FSRDConvShader->IsInit())
        return false;

    if (!FSRDConvShader->SetMaxRenderSize(_denoiserCtxDesc.maxRenderSize.width, _denoiserCtxDesc.maxRenderSize.height))
        return false;

    if ((_denoiserCtxDesc.flags & FFX_DENOISER_ENABLE_DEBUGGING) && !CreateNativeDebugResources())
        return false;
    _diagnostics.nativeDebugAvailable.store(_nativeDebugOutput != nullptr);

    return true;
}

bool FSRDFeatureDx12::CreateNativeDebugResources()
{
    // Allocate once at context capacity. Resolution/view switches never replace a texture
    // or descriptor still referenced by queued GPU work. The debug viewport covers this full target.
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = _denoiserCtxDesc.maxRenderSize.width;
    desc.Height = _denoiserCtxDesc.maxRenderSize.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (FAILED(Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, readable, nullptr,
                                                IID_PPV_ARGS(&_nativeDebugOutput))))
    {
        LOG_ERROR("Cannot allocate AMD native debug output");
        return false;
    }
    _nativeDebugOutput->SetName(L"FSR_RR_NativeDebug");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_nativeDebugCpuHeap))))
        return false;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_nativeDebugGpuHeap))))
        return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = desc.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(_nativeDebugOutput.Get(), nullptr, &uav,
                                      _nativeDebugCpuHeap->GetCPUDescriptorHandleForHeapStart());
    Device->CreateUnorderedAccessView(_nativeDebugOutput.Get(), nullptr, &uav,
                                      _nativeDebugGpuHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void FSRDFeatureDx12::ClearNativeDebugOutput(ID3D12GraphicsCommandList* commandList)
{
    constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    FSRD::AddBarrier(commandList, _nativeDebugOutput.Get(), readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = { _nativeDebugGpuHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    const float clear[4] = {};
    commandList->ClearUnorderedAccessViewFloat(_nativeDebugGpuHeap->GetGPUDescriptorHandleForHeapStart(),
        _nativeDebugCpuHeap->GetCPUDescriptorHandleForHeapStart(), _nativeDebugOutput.Get(), clear, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = _nativeDebugOutput.Get();
    commandList->ResourceBarrier(1, &barrier);
}

bool FSRDFeatureDx12::ShowNativeDebugOutput(ID3D12GraphicsCommandList* commandList,
                                           const NVSDK_NGX_Parameter& parameters)
{
    if (!_frameShowNativeDebug)
        return true;
    ID3D12Resource* output = nullptr;
    if (!TryGetNGXVoidPointer(parameters, NVSDK_NGX_Parameter_Output, output))
        return false;
    const auto& cfg = *Config::Instance();
    const auto outputState = cfg.OutputResourceBarrier.has_value()
        ? D3D12_RESOURCE_STATES(cfg.OutputResourceBarrier.value()) : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    FSRD::AddBarrier(commandList, output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool success = FSRDConvShader->Blit(commandList, _nativeDebugOutput.Get(), output, {}, true);
    FSRD::AddBarrier(commandList, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputState);
    return success;
}

bool FSRDFeatureDx12::QueryDenoiserVersions()
{
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    auto& state = State::Instance();

    // Get version count
    uint64_t versionCount = 0;
    ffxQueryDescGetVersions queryVersionsDesc = { .header = { .type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS },
                                                  .createDescType = FFX_API_EFFECT_ID_DENOISER,
                                                  .device = Device,
                                                  .outputCount = &versionCount };
    FfxApiProxy::D3D12_Query(nullptr, &queryVersionsDesc.header);

    state.ffxDenoiserVersionIds.resize(versionCount);
    state.ffxDenoiserVersionNames.resize(versionCount);

    state.ffxDenoiserDebugModes.clear();
    state.ffxDenoiserDebugModeNames.clear();

    for (const auto& mode : kDebugModes)
    {
        state.ffxDenoiserDebugModes.push_back(mode.second);
        state.ffxDenoiserDebugModeNames.emplace(mode.second, mode.first);
    }

    if (versionCount == 0)
    {
        LOG_ERROR("No FSR-RR denoisers were found.");
        return false;
    }
    else
        LOG_DEBUG("Found {} versions of FSR-RR", versionCount);

    LOG_DEBUG("Initialising FSR denoiser context");

    // Get version IDs
    queryVersionsDesc.versionIds = state.ffxDenoiserVersionIds.data();
    queryVersionsDesc.versionNames = state.ffxDenoiserVersionNames.data();
    if (FfxApiProxy::D3D12_Query(nullptr, &queryVersionsDesc.header) != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Failed to enumerate FSR-RR providers");
        return false;
    }
    auto& index = Config::Instance()->FfxDenoiserIndex;
    if (index.value_or_default() < 0 || static_cast<uint64_t>(index.value_or_default()) >= versionCount)
        index.set_volatile_value(0);
    return true;
}

bool FSRDFeatureDx12::QueryDefaultDenoiserSettings()
{
    const auto result = _denoiser.QueryDefaults();
    if (result.code != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Failed to query FSR-RR setting {}: {}", result.key,
                  FfxApiProxy::ReturnCodeToString(result.code));
        return false;
    }
    return true;
}

void FSRDFeatureDx12::DestroyDenoiserContext()
{
    _denoiser.Destroy();
}

bool FSRDFeatureDx12::UpdateSize(const NVSDK_NGX_Parameter* parameters)
{
    unsigned int width = RenderWidth(), height = RenderHeight();
    GetRenderResolution(parameters, &width, &height);
    if (!FSRD::FitsRenderSize(width, height, _denoiserCtxDesc.maxRenderSize.width,
                              _denoiserCtxDesc.maxRenderSize.height))
    {
        LOG_ERROR("Invalid FSR-RR render size {}x{} (capacity {}x{}); recreate the feature for a larger display", width,
                  height, _denoiserCtxDesc.maxRenderSize.width, _denoiserCtxDesc.maxRenderSize.height);
        return false;
    }

    if (_lastRenderWidth != width || _lastRenderHeight != height)
    {
        LOG_INFO("FSR-RR history reset for render size {}x{}", width, height);
        _hasCameraHistory = false;
        _lastRenderWidth = width;
        _lastRenderHeight = height;
    }
    return true;
}

void FSRDFeatureDx12::OverrideUpscaleDispatch(ffxDispatchDescUpscale& params)
{
    if (_upscaleColorOverride == nullptr)
        return;

    params.color = ffxApiGetResourceDX12(_upscaleColorOverride, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    params.cameraFovAngleVertical = _upscaleFovVertical;
    params.frameTimeDelta = _upscaleDeltaTime;
    params.reset |= _isInReset || _diagnosticUpscaleReset;
}

void FSRDFeatureDx12::CommitCameraHistory()
{
    _prevViewMatrix = _viewMatrix;
    _prevProjMatrix = _projMatrix;
    _lastCamPos = GetFloat3Column(_invViewMatrix, 3);
    _hasCameraHistory = true;
}

bool FSRDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
    {
        _hasCameraHistory = false;
        return false;
    }

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto& inParams = *InParameters;

    if (!FSRD::ValidateInputContract(inParams, cfg, state, _isHWDepth, _isRoughnessPacked))
    {
        _hasCameraHistory = false;
        return false;
    }

    FSRDResearch::Poll();

    if (!UpdateSize(InParameters))
    {
        _hasCameraHistory = false;
        return false;
    }

    if (!FSRD::IsSupportedMotionLayout(JitteredMV(), LowResMV(), RenderWidth(), RenderHeight(), DisplayWidth(),
                                       DisplayHeight()))
    {
        LOG_ERROR("FSR-RR requires non-jittered, render-resolution motion; jittered={} lowRes={} render={}x{} display={}x{}",
                  JitteredMV(), LowResMV(), RenderWidth(), RenderHeight(), DisplayWidth(), DisplayHeight());
        _hasCameraHistory = false;
        return false;
    }

    const auto dbgMode = static_cast<DebugModes>(cfg.FfxDenoiserDebugMode.value_or_default());
    _frameDebugMode = uint64_t(dbgMode);
    const bool diagnosticView = dbgMode == DebugModes::None || dbgMode == DebugModes::UpscalerBypass;
    const uint32_t requestedOptions = _diagnostics.Options();
    _frameDiagnosticOptions = diagnosticView ? requestedOptions : 0;
    const bool isDebugVis = (uint32_t) dbgMode & (uint32_t) DebugModes::ConversionDebug;
    const bool isDebugComp = ((uint64_t) dbgMode & (uint64_t) DebugModes::CompositionDebug);
    const bool hasAnyDebug = (dbgMode != DebugModes::None);

    // Denoise is bypassed if we are debugging something OTHER than the final outputs
    const bool isDenoiseBypassed =
        !isDebugComp && hasAnyDebug && dbgMode != DebugModes::DenoiserOutput && dbgMode != DebugModes::UpscalerBypass;

    // Upscale is bypassed if we are in a debug mode that isn't the DenoiserBypass (final raw)
    const bool isUpscaleBypassed = (hasAnyDebug && dbgMode != DebugModes::DenoiserBypass) ||
                                  (_frameDiagnosticOptions & FSRD::BypassUpscaler) != 0;

    // Validate helper features
    if (!RCAS->IsInit())
        cfg.RcasEnabled.set_volatile_value(false);
    if (!OutputScaler->IsInit())
        cfg.OutputScalingEnabled.set_volatile_value(false);

    _isInReset = !_hasCameraHistory;

    if (uint32_t value = 0; inParams.Get(NVSDK_NGX_Parameter_Reset, &value) == NVSDK_NGX_Result_Success)
        _isInReset |= value > 0;

    // A failed dispatch/composition/upscale must not publish camera or projection history.
    // Only a completely successful RR evaluation commits it below. Output-only debug
    // visualization still runs RR and may keep history; actual denoiser bypasses may not.
    _hasCameraHistory = false;
    const bool denoiserHadHistory = _hasDenoiserHistory;
    _hasDenoiserHistory = false;
    const bool upscalerHadHistory = _hasUpscalerHistory;
    _hasUpscalerHistory = false;

    // Denoiser start
    ffxDispatchDescDenoiserInput1Signal fusedSignal = {};
    ffxDispatchDescDenoiser denoiserDesc = {};
    bool isDenoiserReady = false;

    FSRDCyberpunkFogProbe::CaptureCandidate fogCandidate;
    bool fogCandidateStarted = false;
    struct FogCandidateCompletion
    {
        const FSRDCyberpunkFogProbe::CaptureCandidate& candidate;
        const bool& started;
        ~FogCandidateCompletion() { FSRDCyberpunkFogProbe::CandidateCaptureResult(candidate, started); }
    } fogCandidateCompletion { fogCandidate, fogCandidateStarted };
    if (cfg.FfxDenoiserCyberpunkFogProbe.value_or_default() &&
        cfg.FfxDenoiserCyberpunkFogCapture.value_or_default())
    {
        if (_denoiser.IsCreated())
            FSRDCyberpunkFogProbe::ArmPrivateReset(Device, RenderWidth(), RenderHeight());
        if (_denoiser.IsCreated())
            FSRDCyberpunkFogProbe::ArmRgbIdentity(Device, RenderWidth(), RenderHeight());
        ID3D12Resource* color = nullptr;
        ID3D12Resource* beforeParticles = nullptr;
        TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_Color, color);
        TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, beforeParticles);
        fogCandidate = FSRDCyberpunkFogProbe::ObserveNgxInput(InCommandList, color, beforeParticles,
                                                            Handle()->Id, _frameCount, RenderWidth(), RenderHeight());
    }

    // Pull configuration and input buffers for DLSS-RR from the param table, convert and
    // repack input buffers into intermediate FSR-RR input buffers, and configure descriptors.
    if (!PrepareDenoiserInput(InCommandList, *InParameters, denoiserDesc, fusedSignal))
    {
        _hasCameraHistory = false;
        return false;
    }

    // The options were snapshotted before conversion. Request the dump here on
    // the render thread after input preparation, so the reset and capture cannot
    // land on different frames. A busy capture defers the manual reset request.
    const bool manualDenoiserReset = !fogCandidate && diagnosticView && !(_frameDiagnosticOptions & FSRD::IdentityDenoiser) &&
        _diagnostics.resetDenoiserHistory.load() && FSRDResearch::Request(Handle()->Id);
    const auto diagnosticPlan = FSRD::PlanDiagnostics(_frameDiagnosticOptions, isDenoiseBypassed,
        _isInReset, denoiserHadHistory, _lastDenoiserOptions, upscalerHadHistory, _lastUpscalerOptions,
        manualDenoiserReset);
    _diagnosticUpscaleReset = diagnosticPlan.resetUpscaler;
    if (diagnosticPlan.resetDenoiser)
        denoiserDesc.flags |= FFX_DENOISER_DISPATCH_RESET;

    // Capture effective provider settings, not the previous frame's cached values.
    // Configuration does not alter the converted signal or camera/motion data.
    if (diagnosticPlan.runDenoiser && !ConfigureDenoiser())
        return false;

    const uint32_t nativeDebugSelection = _diagnostics.NativeDebugSelection();
    _frameShowNativeDebug = diagnosticView && diagnosticPlan.runDenoiser && _nativeDebugOutput &&
                            _diagnostics.ShowNativeDebug();
    const bool dispatchNativeDebug = diagnosticView && diagnosticPlan.runDenoiser && _nativeDebugOutput &&
                                     (_frameShowNativeDebug || fogCandidate || FSRDResearch::WantsCapture(Handle()->Id));
    ffxDispatchDescDenoiserDebugView nativeDebugDesc = {};
    if (dispatchNativeDebug)
    {
        nativeDebugDesc.header.type = FFX_API_DISPATCH_DESC_DEBUG_VIEW_TYPE_DENOISER;
        nativeDebugDesc.output = ffxApiGetResourceDX12(_nativeDebugOutput.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        nativeDebugDesc.outputSize = _denoiserCtxDesc.maxRenderSize;
        nativeDebugDesc.mode = nativeDebugSelection == 0 ? FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW
                                                       : FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT;
        nativeDebugDesc.viewportIndex = nativeDebugSelection == 0 ? 0 : nativeDebugSelection - 1;
        // This is an extension of the SAME denoiser dispatch, not a second evaluation.
        fusedSignal.header.pNext = &nativeDebugDesc.header;
    }

    FSRDResearch::Capture research;
    bool captureEvaluationSucceeded = false;
    struct CaptureCompletion
    {
        const FSRDResearch::Capture& capture;
        const bool& succeeded;
        ~CaptureCompletion() { FSRDResearch::Finish(capture, succeeded); }
    } captureCompletion { research, captureEvaluationSucceeded };
    if (fogCandidate || FSRDResearch::WantsCapture(Handle()->Id))
    {
        auto matrix = [](const XMFLOAT4X4& value)
        {
            auto result = nlohmann::json::array();
            for (const auto& row : value.m)
                result.push_back({ row[0], row[1], row[2], row[3] });
            return result;
        };
        XMFLOAT4X4 previousProjection;
        XMStoreFloat4x4(&previousProjection, XMMatrixTranspose(_prevProjMatrix));
        const auto& settings = _denoiser.Settings();
        nlohmann::json metadata = {
            {"render_size", {RenderWidth(), RenderHeight()}},
            {"display_size", {DisplayWidth(), DisplayHeight()}},
            {"debug_mode", uint64_t(dbgMode)},
            {"denoiser_bypassed", isDenoiseBypassed}, {"upscaler_bypassed", isUpscaleBypassed},
            {"identity_denoiser", diagnosticPlan.identity},
            {"diagnostic_options_requested", requestedOptions},
            {"diagnostic_options_active", _frameDiagnosticOptions},
            {"diagnostic_controls_active", diagnosticView},
            {"albedo_divide", !(_frameDiagnosticOptions & FSRD::SkipAlbedoDivide)},
            {"albedo_multiply", !(_frameDiagnosticOptions & FSRD::SkipAlbedoMultiply)},
            {"add_residual", !(_frameDiagnosticOptions & FSRD::SkipResidual)},
            {"denoiser_executed", diagnosticPlan.runDenoiser},
            {"denoiser_output_kind", diagnosticPlan.identity ? "identity_converted_input" :
                                      isDenoiseBypassed ? "absent" : "amd_filtered"},
            {"diagnostic_manual_reset", manualDenoiserReset},
            {"denoiser_reset", diagnosticPlan.resetDenoiser},
            {"denoiser_history_valid_on_entry", denoiserHadHistory},
            {"bridge_upscaler_reset", !isUpscaleBypassed && diagnosticPlan.resetUpscaler},
            {"output_stage", "backend output before OptiScaler sharpening/output scaling/overlay"},
            {"reset", _isInReset}, {"hw_depth", _isHWDepth}, {"inverted_depth", DepthInverted()},
            {"packed_roughness", _isRoughnessPacked},
            {"pipeline", "pure_fused"},
            {"floor_isolation", 0.0f},
            {"correlation_bias", 0.0f},
            {"near", _convDesc.NearPlane}, {"far", _convDesc.FarPlane},
            {"conversion_flags", _convDesc.Flags},
            {"inv_view", matrix(_convDesc.InvViewMatrix)},
            {"inv_projection", matrix(_convDesc.InvProjMatrix)},
            {"previous_view", matrix(_convDesc.PrevViewMatrix)},
            {"previous_projection", matrix(previousProjection)},
            {"previous_depth_projection", {_convDesc.PreviousDepthProjection.x,
                                             _convDesc.PreviousDepthProjection.y,
                                             _convDesc.PreviousDepthProjection.z}},
            {"motion_depth_encoding", (_convDesc.Flags & (uint32_t)FSRDConvFlags::CyberpunkDepthMotion)
                                           ? "cyberpunk_hardware_delta_1000" : "camera_only"},
            {"amd_jitter", {denoiserDesc.jitterOffsets.x, denoiserDesc.jitterOffsets.y}},
            {"amd_motion_scale", {denoiserDesc.motionVectorScale.x, denoiserDesc.motionVectorScale.y,
                                    denoiserDesc.motionVectorScale.z}},
            {"amd_dispatch", {
                {"delta_time_ms", denoiserDesc.deltaTime}, {"frame_index", denoiserDesc.frameIndex},
                {"flags", denoiserDesc.flags},
                {"render_size", {denoiserDesc.renderSize.width, denoiserDesc.renderSize.height}},
                {"motion_scale", {denoiserDesc.motionVectorScale.x, denoiserDesc.motionVectorScale.y,
                                    denoiserDesc.motionVectorScale.z}},
                {"jitter", {denoiserDesc.jitterOffsets.x, denoiserDesc.jitterOffsets.y}},
                {"camera_position_delta", {denoiserDesc.cameraPositionDelta.x, denoiserDesc.cameraPositionDelta.y,
                                             denoiserDesc.cameraPositionDelta.z}},
                {"camera_right", {denoiserDesc.cameraRight.x, denoiserDesc.cameraRight.y, denoiserDesc.cameraRight.z}},
                {"camera_up", {denoiserDesc.cameraUp.x, denoiserDesc.cameraUp.y, denoiserDesc.cameraUp.z}},
                {"camera_forward", {denoiserDesc.cameraForward.x, denoiserDesc.cameraForward.y,
                                      denoiserDesc.cameraForward.z}},
                {"camera_aspect_ratio", denoiserDesc.cameraAspectRatio},
                {"camera_near", denoiserDesc.cameraNear}, {"camera_far", denoiserDesc.cameraFar},
                {"camera_fov_vertical", denoiserDesc.cameraFovAngleVertical}
            }},
            {"amd_settings", {
                {"1", settings.crossBilateralNormalStrength}, {"2", settings.stabilityBias},
                {"3", settings.maxRadiance}, {"4", settings.radianceClipStdK},
                {"5", settings.gaussianKernelRelaxation}, {"6", settings.disocclusionThreshold}
            }},
            {"amd_provider_id", _denoiserProviderId},
            {"amd_provider_version", _denoiserProviderName},
            {"amd_context_flags", _denoiserCtxDesc.flags},
            {"amd_max_render_size", {_denoiserCtxDesc.maxRenderSize.width, _denoiserCtxDesc.maxRenderSize.height}},
            {"amd_native_debug", {
                {"context_enabled", (_denoiserCtxDesc.flags & FFX_DENOISER_ENABLE_DEBUGGING) != 0},
                {"dispatched", dispatchNativeDebug}, {"displayed", _frameShowNativeDebug},
                {"mode", nativeDebugDesc.mode}, {"viewport_index", nativeDebugDesc.viewportIndex},
                {"output_size", {nativeDebugDesc.outputSize.width, nativeDebugDesc.outputSize.height}},
                {"format", "RGBA16_FLOAT; alpha is provider write coverage"}
            }}
        };
        for (const char* key : { NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
                NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y, NVSDK_NGX_Parameter_DLSS_Pre_Exposure })
        {
            float value;
            if (inParams.Get(key, &value) == NVSDK_NGX_Result_Success)
                metadata["scalars"][key] = value;
        }
        if (fogCandidate)
        {
            // Candidate identity is not a validated same-frame association. Its submission
            // sidecar and exact image values must be checked offline before pairing.
            metadata["fog_candidate"] = nlohmann::json::parse(fogCandidate->metadata);
            research = FSRDResearch::BeginFogCandidate(Device, InCommandList, RenderWidth(), RenderHeight(),
                Handle()->Id, _frameCount, metadata.dump(), fogCandidate->ownership);
            fogCandidateStarted = bool(research);
        }
        else
            research = FSRDResearch::Begin(Device, InCommandList, RenderWidth(), RenderHeight(),
                                          Handle()->Id, _frameCount, metadata.dump());
        if (research)
        {
            const std::pair<const char*, const char*> inputs[] = {
                {"input_color", NVSDK_NGX_Parameter_Color},
                {"input_depth", NVSDK_NGX_Parameter_Depth},
                {"input_motion", NVSDK_NGX_Parameter_MotionVectors},
                {"input_normal_roughness", NVSDK_NGX_Parameter_GBuffer_Normals},
                {"input_roughness", NVSDK_NGX_Parameter_GBuffer_Roughness},
                {"input_diffuse_albedo", NVSDK_NGX_Parameter_DiffuseAlbedo},
                {"input_specular_albedo", NVSDK_NGX_Parameter_SpecularAlbedo},
                {"input_specular_hit_distance", NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance},
                {"input_diffuse_hit_distance", NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance},
                {"input_specular_motion", NVSDK_NGX_Parameter_GBuffer_SpecularMvec},
                {"input_motion_3d", NVSDK_NGX_Parameter_MotionVectors3D},
                {"input_specular_direction", NVSDK_NGX_Parameter_DLSSD_SpecularRayDirection},
                {"input_specular_direction_hit", NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance},
                {"input_diffuse_direction_hit", NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance},
                {"input_diffuse_direction", NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirection},
                {"input_reflected_albedo", NVSDK_NGX_Parameter_DLSSD_ReflectedAlbedo},
                {"input_before_particles", NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles},
                {"input_after_particles", NVSDK_NGX_Parameter_DLSSD_ColorAfterParticles},
                {"input_before_transparency", NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency},
                {"input_after_transparency", NVSDK_NGX_Parameter_DLSSD_ColorAfterTransparency},
                {"input_before_fog", NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog},
                {"input_after_fog", NVSDK_NGX_Parameter_DLSSD_ColorAfterFog},
                {"input_sss_guide", NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide},
                {"input_before_sss", NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering},
                {"input_after_sss", NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceSubsurfaceScattering},
                {"input_refraction_guide", NVSDK_NGX_Parameter_DLSSD_ScreenSpaceRefractionGuide},
                {"input_before_refraction", NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceRefraction},
                {"input_after_refraction", NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceRefraction},
                {"input_dof_guide", NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide},
                {"input_before_dof", NVSDK_NGX_Parameter_DLSSD_ColorBeforeDepthOfField},
                {"input_after_dof", NVSDK_NGX_Parameter_DLSSD_ColorAfterDepthOfField},
                {"input_alpha", NVSDK_NGX_Parameter_DLSSD_Alpha},
                {"input_emissive", NVSDK_NGX_Parameter_GBuffer_Emissive},
                {"input_transparency", NVSDK_NGX_Parameter_DLSS_TransparencyLayer},
                {"input_transparency_opacity", NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity},
                {"input_transparency_motion", NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs},
                {"input_bias", NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask},
                {"input_exposure", NVSDK_NGX_Parameter_ExposureTexture}
            };
            for (const auto& [name, key] : inputs)
            {
                ID3D12Resource* resource = nullptr;
                TryGetNGXVoidPointer(inParams, key, resource);
                FSRDResearch::Record(research, name, resource);
            }
            FSRDResearch::Record(research, "converted_radiance", GetD3D12ResFromFFX(fusedSignal.radiance.input));
            FSRDResearch::Record(research, "converted_fused_albedo", GetD3D12ResFromFFX(fusedSignal.fusedAlbedo));
            FSRDResearch::Record(research, "converted_motion", GetD3D12ResFromFFX(denoiserDesc.motionVectors));
            FSRDResearch::Record(research, "converted_normals", GetD3D12ResFromFFX(denoiserDesc.normals));
            FSRDResearch::Record(research, "converted_depth", GetD3D12ResFromFFX(denoiserDesc.linearDepth));
            FSRDResearch::Record(research, "converted_diffuse_albedo", GetD3D12ResFromFFX(denoiserDesc.diffuseAlbedo));
            FSRDResearch::Record(research, "converted_specular_albedo", GetD3D12ResFromFFX(denoiserDesc.specularAlbedo));
            FSRDResearch::Record(research, "preserved_lighting", FSRDConvShader->GetPreservedLighting());
        }
    }

    // Dispatch denoiser
    if (!isDenoiseBypassed)
    {
        if (diagnosticPlan.identity)
        {
            // No shader, sampling or copy intervenes. The normal compositor reads
            // converted radiance directly; all of its other inputs are unchanged.
            isDenoiserReady = true;
            FSRDResearch::Record(research, "denoised_radiance", GetD3D12ResFromFFX(fusedSignal.radiance.input));
        }
        else
        {
            if (dispatchNativeDebug)
                ClearNativeDebugOutput(InCommandList);
            FSRDConvShader->SetDenoiserOutputsWritable(InCommandList, true);
            isDenoiserReady = DispatchDenoiser(InCommandList, denoiserDesc);
            FSRDConvShader->SetDenoiserOutputsWritable(InCommandList, false);
            if (dispatchNativeDebug)
            {
                constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                FSRD::AddBarrier(InCommandList, _nativeDebugOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
                if (isDenoiserReady)
                    FSRDResearch::Record(research, "amd_native_debug", _nativeDebugOutput.Get(), true);
            }
            FSRDResearch::Record(research, "denoised_radiance", GetD3D12ResFromFFX(fusedSignal.radiance.output));
            if (isDenoiserReady && manualDenoiserReset)
            {
                _diagnostics.resetDenoiserHistory.store(false);
                _diagnostics.lastResetFrame.store(_frameCount);
                LOG_INFO("FSR-RR diagnostic denoiser-only reset recorded at frame {}", _frameCount);
            }
        }

        if (!isDenoiserReady)
        {
            _hasCameraHistory = false;
            return false;
        }

        // Compose denoised signals
        FSRDCompDesc compDesc = { .DstTexSize = _convDesc.RenderSize,
                                  .Flags = (uint32_t) GetCompDebugFlags(dbgMode) };
        if (_frameDiagnosticOptions & FSRD::SkipAlbedoMultiply)
            compDesc.Flags |= (uint32_t) FSRDCompFlags::SkipAlbedoMultiply;
        if (_frameDiagnosticOptions & FSRD::SkipResidual)
            compDesc.Flags |= (uint32_t) FSRDCompFlags::SkipResidual;

        if (!FSRDConvShader->DispatchComposition(InCommandList, compDesc, diagnosticPlan.identity))
            return false;

        FSRDResearch::Record(research, "composed_color", FSRDConvShader->GetCompositionOutput());

        isDenoiserReady = true;
    }
    else
    {
        _hasCameraHistory = false; // Resuming after a debug bypass must reset stale denoiser history.
        FSRDResearch::Record(research, "denoised_radiance", nullptr);
        FSRDResearch::Record(research, "composed_color", nullptr);
    }

    // Upscaler start
    if (!isUpscaleBypassed)
    {
        // The base implementation gathers the upscaler inputs, applies the configurable resource
        // barriers and dispatches. OverrideUpscaleDispatch() swaps in the composited denoiser output.
        _upscaleColorOverride = isDenoiserReady ? FSRDConvShader->GetCompositionOutput() : nullptr;
        _upscaleFovVertical = denoiserDesc.cameraFovAngleVertical;
        _upscaleDeltaTime = denoiserDesc.deltaTime;

        // Post-processing (RCAS / output scaling / ImGui) is handled by IFeature_Dx12::Evaluate
        const bool isUpscalerReady = FFXFeatureDx12::EvaluateInternal(InCommandList, InParameters);

        _upscaleColorOverride = nullptr;

        if (research && isUpscalerReady)
        {
            ID3D12Resource* output = nullptr;
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_Output, output);
            const auto outputState = cfg.OutputResourceBarrier.has_value()
                ? D3D12_RESOURCE_STATES(cfg.OutputResourceBarrier.value()) : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            FSRDResearch::RecordOutput(research, output, outputState);
        }

        // Preserve/capture the normal backend result and SR history first. Only an explicit
        // GUI display request replaces the visible output; raw AMD debug is captured separately.
        if (isUpscalerReady && !ShowNativeDebugOutput(InCommandList, inParams))
            return false;

        if (isUpscalerReady && isDenoiserReady)
        {
            CommitCameraHistory();
            _hasDenoiserHistory = diagnosticPlan.runDenoiser;
            _lastDenoiserOptions = _frameDiagnosticOptions;
        }
        if (isUpscalerReady)
        {
            _hasUpscalerHistory = true;
            _lastUpscalerOptions = _frameDiagnosticOptions;
        }

        // _frameCount is incremented by the base implementation
        captureEvaluationSucceeded = isUpscalerReady;
        return isUpscalerReady;
    }
    else // Debug visualization
    {
        ID3D12Resource* srcTex = nullptr;

        if (dbgMode == DebugModes::DlssColorBeforeParticles)
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, srcTex);
        else if (dbgMode == DebugModes::DlssColorBeforeTransparency)
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, srcTex);
        else if (dbgMode == DebugModes::DlssTransparencyLayer)
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSS_TransparencyLayer, srcTex);
        else if (dbgMode == DebugModes::DlssBias)
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, srcTex);
        else if (dbgMode == DebugModes::RawColor)
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_Color, srcTex);
        else if (isDebugVis)
            srcTex = GetD3D12ResFromFFX(fusedSignal.radiance.input);
        else
            srcTex = FSRDConvShader->GetCompositionOutput();

        ID3D12Resource* dstTex;

        if (!srcTex || !TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Output, dstTex))
        {
            _frameCount++;
            return false;
        }

        if (!FSRDConvShader->Blit(InCommandList, srcTex, dstTex))
            return false;
        FSRDResearch::RecordOutput(research, dstTex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (_frameShowNativeDebug && !FSRDConvShader->Blit(InCommandList, _nativeDebugOutput.Get(), dstTex, {}, true))
            return false;
        if (isDenoiserReady)
        {
            CommitCameraHistory();
            _hasDenoiserHistory = diagnosticPlan.runDenoiser;
            _lastDenoiserOptions = _frameDiagnosticOptions;
            // SR remains invalid until it actually executes again.
        }
    }

    _frameCount++;
    captureEvaluationSucceeded = isDenoiserReady || isDenoiseBypassed;
    return captureEvaluationSucceeded;
}

bool FSRDFeatureDx12::PrepareDenoiserInput(ID3D12GraphicsCommandList* InCommandList,
                                           const NVSDK_NGX_Parameter& inParams, ffxDispatchDescDenoiser& dispatchDesc,
                                           ffxDispatchDescDenoiserInput1Signal& signalDesc)
{
    const auto& cfg = *Config::Instance();

    // Keep an independent clock: the subsequent SR evaluation can update its own timer.
    const double now = Util::MillisecondsNow();
    const float measuredDeltaTime = static_cast<float>(now - _lastDenoiserFrameTime);
    _lastDenoiserFrameTime = now;

    float deltaTime = 0.0f;
    if (!TryGetToggleableNGXParam(inParams, OptiKeys::FSR_FrameTimeDelta, cfg.FsrUseFsrInputValues, deltaTime) ||
        !std::isfinite(deltaTime) || deltaTime <= 0.0f)
    {
        if (inParams.Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &deltaTime) != NVSDK_NGX_Result_Success ||
            !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            deltaTime = measuredDeltaTime;
    }
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
    {
        LOG_ERROR("FSR-RR has no positive finite frame time");
        return false;
    }

    const auto ReadOptionalFiniteScalar = [&](const char* key, float& value)
    {
        float incoming = value;
        if (inParams.Get(key, &incoming) == NVSDK_NGX_Result_Success)
            value = incoming;
        if (std::isfinite(value))
            return true;
        LOG_ERROR("FSR-RR received a nonfinite scalar: {}", key);
        return false;
    };

    // NGX scales each motion component into pixels. Missing components independently default
    // to one pixel multiplier, not one UV multiplier; AMD requires pixels divided by render size.
    float MVScaleX = 1.0f, MVScaleY = 1.0f, jitterX = 0.0f, jitterY = 0.0f;
    if (!ReadOptionalFiniteScalar(NVSDK_NGX_Parameter_MV_Scale_X, MVScaleX) ||
        !ReadOptionalFiniteScalar(NVSDK_NGX_Parameter_MV_Scale_Y, MVScaleY) ||
        !ReadOptionalFiniteScalar(NVSDK_NGX_Parameter_Jitter_Offset_X, jitterX) ||
        !ReadOptionalFiniteScalar(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterY))
        return false;

    const FfxApiFloatCoords3D motionScale = { MVScaleX / RenderWidth(), MVScaleY / RenderHeight(), 1.0f };
    // Despite the old RR 1.1 header comment, AMD's SDK 2.2 sample uses projection jitter in NDC.
    const FfxApiFloatCoords2D jitter = { 2.0f * (jitterX / RenderWidth()), -2.0f * (jitterY / RenderHeight()) };
    if (!std::isfinite(motionScale.x) || !std::isfinite(motionScale.y) || !std::isfinite(jitter.x) ||
        !std::isfinite(jitter.y))
    {
        LOG_ERROR("FSR-RR motion/jitter conversion produced nonfinite values");
        return false;
    }

    // Gather DLSS-RR input buffers for conversion and repacking for FSR-RR
    if (!PrepareDenoiseConvInput(inParams))
        return false;

    if (!ConvertDenoiserBuffers(InCommandList))
        return false;

    // Camera matrix - translation and rotation, from viewMatrix^-1
    const XMVECTOR right = XMVector3Normalize(GetColumn(_invViewMatrix, 0));
    const XMVECTOR up = XMVector3Normalize(GetColumn(_invViewMatrix, 1));
    const XMVECTOR forward = XMVectorScale(XMVector3Normalize(GetColumn(_invViewMatrix, 2)),
                                           _projMatrix.r[3].m128_f32[2] < 0.0f ? -1.0f : 1.0f);
    const XMFLOAT3 camPos = GetFloat3Column(_invViewMatrix, 3);
    const XMFLOAT3 previousCamPos = _isInReset ? camPos : _lastCamPos;

    // Pack dispatch configuration
    dispatchDesc = {
        .commandList = InCommandList,
        .motionVectorScale = motionScale,
        .jitterOffsets = jitter,
        // Camera movement since last frame (PreviousPosition - CurrentPosition)
        .cameraPositionDelta = { previousCamPos.x - camPos.x, previousCamPos.y - camPos.y,
                                 previousCamPos.z - camPos.z },
        .cameraRight = GetFloat3FFX(right),
        .cameraUp = GetFloat3FFX(up),
        .cameraForward = GetFloat3FFX(forward),
        .cameraAspectRatio = GetAspectRatioFromProjectionMatrix(_projMatrix),
        .cameraNear = _convDesc.NearPlane,
        .cameraFar = _convDesc.FarPlane,
        .cameraFovAngleVertical = GetVertFovFromProjectionMatrixRad(_projMatrix),
        .renderSize = { RenderWidth(), RenderHeight() },
        .deltaTime = deltaTime,
        .frameIndex = (uint32_t) _frameCount,
        .flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO
    };

    // Populate resources and link signal header
    FSRDConvShader->GetSignal(signalDesc, dispatchDesc);

    if (_isInReset)
        dispatchDesc.flags |= FFX_DENOISER_DISPATCH_RESET;

    LOG_DEBUG("Jitter NDC [{:.6f}, {:.6f}]", dispatchDesc.jitterOffsets.x, dispatchDesc.jitterOffsets.y);

    CaptureInputs(inParams, dispatchDesc);

    return true;
}

void FSRDFeatureDx12::CaptureInputs(const NVSDK_NGX_Parameter& inParams, const ffxDispatchDescDenoiser& dispatchDesc)
{
    const auto limit =
        static_cast<uint32_t>(std::clamp(Config::Instance()->FfxDenoiserCaptureSamples.value_or_default(), 0, 240));
    if (_captureSamples >= limit || (_frameCount > 1 && _frameCount % 30 != 0))
        return;

    ++_captureSamples;
    const auto& slData = State::Instance().slLastConstants;
    LOG_INFO("FSRRR_CAPTURE begin handle={} frame={} sample={}/{} hwDepth={} inverted={} packedRoughness={} reset={}",
             Handle()->Id, _frameCount, _captureSamples, limit, _isHWDepth, DepthInverted(), _isRoughnessPacked,
             _isInReset);

    const auto LogMatrix = [](const char* name, const XMMATRIX& matrix)
    {
        XMFLOAT4X4 values;
        XMStoreFloat4x4(&values, matrix);
        for (int row = 0; row < 4; ++row)
            LOG_INFO("FSRRR_CAPTURE matrix {} row={} {:.9g} {:.9g} {:.9g} {:.9g}", name, row, values.m[row][0],
                     values.m[row][1], values.m[row][2], values.m[row][3]);
    };
    const auto LogRawMatrix = [&](const char* key)
    {
        XMMATRIX matrix;
        // Deliberately preserve the incoming 16-float memory layout before any transpose.
        if (TryGetNGXMatrixTranspose(inParams, key, matrix))
            LogMatrix(key, matrix);
        else
            LOG_INFO("FSRRR_CAPTURE matrix {} missing", key);
    };
    LogRawMatrix(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX);
    LogRawMatrix(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX);
    LogMatrix("derivedView", _viewMatrix);
    LogMatrix("derivedInvView", _invViewMatrix);
    LogMatrix("derivedPrevView", _prevViewMatrix);
    LogMatrix("derivedProjection", _projMatrix);
    XMMATRIX slProjection;
    static_assert(sizeof(slData.cameraViewToClip) == sizeof(slProjection));
    memcpy(&slProjection, &slData.cameraViewToClip, sizeof(slProjection));
    LogMatrix("slCameraViewToClip", slProjection);

    const auto LogVector = [](const char* name, float x, float y, float z)
    { LOG_INFO("FSRRR_CAPTURE vector {} {:.9g} {:.9g} {:.9g}", name, x, y, z); };
    LogVector("slPosition", slData.cameraPos.x, slData.cameraPos.y, slData.cameraPos.z);
    LogVector("slForward", slData.cameraFwd.x, slData.cameraFwd.y, slData.cameraFwd.z);
    LogVector("slRight", slData.cameraRight.x, slData.cameraRight.y, slData.cameraRight.z);
    LogVector("slUp", slData.cameraUp.x, slData.cameraUp.y, slData.cameraUp.z);
    LogVector("amdForward", dispatchDesc.cameraForward.x, dispatchDesc.cameraForward.y, dispatchDesc.cameraForward.z);
    LogVector("amdRight", dispatchDesc.cameraRight.x, dispatchDesc.cameraRight.y, dispatchDesc.cameraRight.z);
    LogVector("amdUp", dispatchDesc.cameraUp.x, dispatchDesc.cameraUp.y, dispatchDesc.cameraUp.z);
    LogVector("amdPositionDelta", dispatchDesc.cameraPositionDelta.x, dispatchDesc.cameraPositionDelta.y,
              dispatchDesc.cameraPositionDelta.z);
    LogVector("amdMotionScale", dispatchDesc.motionVectorScale.x, dispatchDesc.motionVectorScale.y,
              dispatchDesc.motionVectorScale.z);
    LOG_INFO("FSRRR_CAPTURE amd render={}x{} jitter={:.9g},{:.9g} near={:.9g} far={:.9g} fov={:.9g} dt={:.9g}",
             dispatchDesc.renderSize.width, dispatchDesc.renderSize.height, dispatchDesc.jitterOffsets.x,
             dispatchDesc.jitterOffsets.y, dispatchDesc.cameraNear, dispatchDesc.cameraFar,
             dispatchDesc.cameraFovAngleVertical, dispatchDesc.deltaTime);
    LOG_INFO("FSRRR_CAPTURE streamline near={:.9g} far={:.9g} fov={:.9g} aspect={:.9g}", slData.cameraNear,
             slData.cameraFar, slData.cameraFOV, slData.cameraAspectRatio);

    for (const char* key :
         { NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y, NVSDK_NGX_Parameter_MV_Scale_X,
           NVSDK_NGX_Parameter_MV_Scale_Y, NVSDK_NGX_Parameter_DLSS_Pre_Exposure })
    {
        float value = 0.0f;
        if (inParams.Get(key, &value) == NVSDK_NGX_Result_Success)
            LOG_INFO("FSRRR_CAPTURE scalar {} {:.9g}", key, value);
        else
            LOG_INFO("FSRRR_CAPTURE scalar {} missing", key);
    }
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                             NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height })
    {
        unsigned int value = 0;
        if (inParams.Get(key, &value) == NVSDK_NGX_Result_Success)
            LOG_INFO("FSRRR_CAPTURE dimension {} {}", key, value);
    }
    for (const char* key :
         { NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
           NVSDK_NGX_Parameter_GBuffer_Normals, NVSDK_NGX_Parameter_GBuffer_Roughness,
           NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance })
    {
        ID3D12Resource* resource = nullptr;
        if (TryGetNGXVoidPointer(inParams, key, resource))
        {
            const auto desc = resource->GetDesc();
            LOG_INFO("FSRRR_CAPTURE resource {} {}x{} format={} flags={}", key, desc.Width, desc.Height,
                     static_cast<unsigned int>(desc.Format), static_cast<unsigned int>(desc.Flags));
        }
        else
            LOG_INFO("FSRRR_CAPTURE resource {} missing", key);
    }
    LOG_INFO("FSRRR_CAPTURE end handle={} frame={}", Handle()->Id, _frameCount);
}

bool FSRDFeatureDx12::PrepareDenoiseConvInput(const NVSDK_NGX_Parameter& inParams)
{
    const auto& slData = State::Instance().slLastConstants;

    // Gather DLSS-RR input buffers for conversion and repacking for FSR-RR
    bool isReady = true;
    _convDesc.Resources = {}; // Optional resources must not retain pointers from a previous frame.

    // Standard TSR buffers
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Color, _convDesc.Resources.InColor))
        isReady = false;
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_MotionVectors, _convDesc.Resources.InMotionVectors))
        isReady = false;
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Depth, _convDesc.Resources.InDepth))
        isReady = false;

    // DLSSD-specific buffers
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_GBuffer_Normals, _convDesc.Resources.InNormals))
        isReady = false;

    // If roughness is not packed into normals, then this texture is mandatory.
    // This value should be available in one of these two buffers in any DLSS-RR implementation.
    if (!_isRoughnessPacked &&
        !TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_GBuffer_Roughness, _convDesc.Resources.InRoughness))
        isReady = false;

    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_DiffuseAlbedo, _convDesc.Resources.InDiffAlbedo))
        isReady = false;

    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_SpecularAlbedo, _convDesc.Resources.InSpecAlbedo))
        isReady = false;

    // Optional in both modes. A null SRV reads zero; do not report it as a missing required input.
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, _convDesc.Resources.InSpecHitDist);

    if (!isReady)
        return false;

    // This converter currently consumes origin-zero, render-resolution inputs.
    // Reject undersized resources rather than allowing out-of-bounds reads.
    for (auto* resource : _convDesc.Resources.AsArray)
    {
        if (!resource)
            continue;
        const auto desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width < RenderWidth() ||
            desc.Height < RenderHeight() || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
        {
            LOG_ERROR("FSR-RR needs single-sample, shader-readable 2D inputs covering render size {}x{}", RenderWidth(),
                      RenderHeight());
            return false;
        }
    }

    // Get DLSSD matrices and derive related values
    // World to view/camera space (V)
    _viewMatrix = {};

    if (!TryGetNGXMatrix(inParams, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, _viewMatrix))
    {
        LOG_DEBUG("View matrix missing! Falling back to Streamline inputs...");

        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraRight), 0, _invViewMatrix);
        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraUp), 1, _invViewMatrix);
        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraFwd), 2, _invViewMatrix);
        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraPos), 3, _invViewMatrix);
        _invViewMatrix.r[3].m128_f32[3] = 1.0f;

        if (!IsUsableMatrix(_invViewMatrix))
        {
            LOG_ERROR("FSR-RR requires a valid NGX view matrix or Streamline camera basis");
            return false;
        }

        _viewMatrix = XMMatrixInverse(nullptr, _invViewMatrix);
    }
    else
    {
        // Camera rotation and position
        if (!IsUsableMatrix(_viewMatrix))
        {
            LOG_ERROR("FSR-RR received an invalid view matrix");
            return false;
        }
        _invViewMatrix = XMMatrixInverse(nullptr, _viewMatrix);
    }

    // Perspective projection matrix (P)
    _projMatrix = {};

    if (!TryGetNGXMatrix(inParams, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, _projMatrix))
    {
        LOG_DEBUG("Projection matrix missing! Falling back to Streamline inputs...");

        if (std::isfinite(slData.cameraFOV) && slData.cameraFOV > 0.0f && slData.cameraFOV < 180.0f &&
            std::isfinite(slData.cameraNear) && slData.cameraNear > 0.0f && std::isfinite(slData.cameraFar) &&
            slData.cameraFar < 1e30f && slData.cameraFar > slData.cameraNear &&
            std::isfinite(slData.cameraAspectRatio) && slData.cameraAspectRatio > 0.0f &&
            slData.cameraAspectRatio < 100.0f && std::abs(slData.cameraViewToClip[2].w) == 1.0f)
        {
            // These measurements are supposed to be in radians, but some titles supply degrees.
            // Valid FOV in radians never exceeds PI. Realistic FOV in degrees is basically never in the single digits.
            const float fov = (slData.cameraFOV < 4.0f) ? slData.cameraFOV : GetRadiansFromDeg(slData.cameraFOV);
            const float nearPlane = DepthInverted() ? slData.cameraFar : slData.cameraNear;
            const float farPlane = DepthInverted() ? slData.cameraNear : slData.cameraFar;
            const bool isRightHanded = slData.cameraViewToClip[2].w < 0.0f;

            // Reconstruct a perspective matrix from validated Streamline camera measurements.
            if (isRightHanded)
                _projMatrix = XMMatrixPerspectiveFovRH(fov, slData.cameraAspectRatio, nearPlane, farPlane);
            else
                _projMatrix = XMMatrixPerspectiveFovLH(fov, slData.cameraAspectRatio, nearPlane, farPlane);

            _projMatrix = XMMatrixTranspose(_projMatrix);
        }
    }

    if (!IsUsableMatrix(_projMatrix))
    {
        LOG_ERROR("FSR-RR requires a valid perspective projection matrix");
        return false;
    }
    const auto planes = GetViewPlanes(_projMatrix, DepthInverted());
    if (!(planes.nearPlane > 0.0f && planes.farPlane > planes.nearPlane) || !std::isfinite(planes.nearPlane) ||
        !std::isfinite(planes.farPlane) || _projMatrix.r[0].m128_f32[0] <= 0.0f ||
        _projMatrix.r[1].m128_f32[1] <= 0.0f || std::abs(_projMatrix.r[3].m128_f32[2]) != 1.0f)
    {
        LOG_ERROR("FSR-RR received an unsupported projection or depth convention");
        return false;
    }
    if (_isInReset)
    {
        _prevViewMatrix = _viewMatrix;
        _prevProjMatrix = _projMatrix;
    }
    return true;
}

bool FSRDFeatureDx12::ConvertDenoiserBuffers(ID3D12GraphicsCommandList* InCommandList)
{
    const uint32_t dbgMode = (uint32_t) _frameDebugMode;

    // Prepare input converter
    _convDesc.RenderSize = { (float) RenderWidth(), (float) RenderHeight(), 1.0f / (float) RenderWidth(),
                             1.0f / (float) RenderHeight() };
    _convDesc.Flags = (uint32_t) FSRDConvFlags::NonGammaAlbedo | (dbgMode & (uint32_t) FSRDConvFlags::DebugModeMask);
    if (_frameDiagnosticOptions & FSRD::SkipAlbedoDivide)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::SkipAlbedoDivide;

    if (_isRoughnessPacked)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::IsRoughnessPacked;

    // Store in column major order for GPU
    XMStoreFloat4x4(&_convDesc.InvViewMatrix, XMMatrixTranspose(_invViewMatrix));

    // Inverse perspective projection
    const XMMATRIX invProjMatrix = XMMatrixInverse(nullptr, _projMatrix);
    XMStoreFloat4x4(&_convDesc.InvProjMatrix, XMMatrixTranspose(invProjMatrix));

    // Previous world to view for linear depth delta
    XMStoreFloat4x4(&_convDesc.PrevViewMatrix, XMMatrixTranspose(_prevViewMatrix));
    _convDesc.PreviousDepthProjection = { _prevProjMatrix.r[2].m128_f32[2], _prevProjMatrix.r[2].m128_f32[3],
                                           _prevProjMatrix.r[3].m128_f32[2], 0.0f };

    // Near and far planes
    const ViewPlanes planes = GetViewPlanes(_projMatrix, DepthInverted());
    _convDesc.NearPlane = planes.nearPlane;
    _convDesc.FarPlane = planes.farPlane;

    if (planes.isRightHanded)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::IsRightHanded;

    if (!_isHWDepth)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::IsDepthLinear;

    if (_isInReset)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::ResetMotionHistory;
    else if (_isHWDepth && _stricmp(State::Instance().gameExe.c_str(), "Cyberpunk2077.exe") == 0 &&
             _convDesc.Resources.InMotionVectors->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
             HasSeparablePerspectiveDepth(_projMatrix) && HasSeparablePerspectiveDepth(_prevProjMatrix))
    {
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::CyberpunkDepthMotion;
        if (!_loggedCyberpunkDepthMotion)
        {
            LOG_INFO("FSR-RR using verified Cyberpunk engine depth motion (1000x hardware delta; previous projection)");
            _loggedCyberpunkDepthMotion = true;
        }
    }

    LOG_DEBUG("Distpaching FSRD Input Converter");

    // Dispatch resource converter. Outputs are automatically transitioned for reading.
    if (!FSRDConvShader->DispatchConversion(InCommandList, _convDesc))
        return false;

    return true;
}

bool FSRDFeatureDx12::ConfigureDenoiser()
{
    const auto& cfg = *Config::Instance();
    const FSRD::DenoiserSettings requested = {
        .crossBilateralNormalStrength = cfg.FfxDenoiserCrossBlNormStr.value_or_default(),
        .stabilityBias = cfg.FfxDenoiserStabilityBias.value_or_default(),
        .maxRadiance = cfg.FfxDenoiserMaxRadiance.value_or_default(),
        .radianceClipStdK = cfg.FfxDenoiserRadianceClip.value_or_default(),
        .gaussianKernelRelaxation = cfg.FfxDenoiserGaussKernRelax.value_or_default(),
        .disocclusionThreshold = cfg.FfxDenoiserDisocclusionThreshold.value_or_default()
    };
    const auto result = _denoiser.Configure(requested);
    if (result.code != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Failed to configure FSR-RR setting {}: {}", result.key,
                  FfxApiProxy::ReturnCodeToString(result.code));
        return false;
    }
    return true;
}

bool FSRDFeatureDx12::DispatchDenoiser(ID3D12GraphicsCommandList* InCommandList,
                                       const ffxDispatchDescDenoiser& dispatchDesc)
{
    auto& state = State::Instance();
    LOG_DEBUG("Dispatching FSR-RR...");
    const ffxReturnCode_t result = _denoiser.Dispatch(dispatchDesc);

    if (result != FFX_API_RETURN_OK)
    {
        LOG_ERROR("_dispatch error: {0}", FfxApiProxy::ReturnCodeToString(result));

        if (result == FFX_API_RETURN_ERROR_RUNTIME_ERROR)
        {
            LOG_WARN("Trying to recover by recreating the feature");
            state.changeBackend[Handle()->Id] = true;
        }

        return false;
    }

    return true;
}

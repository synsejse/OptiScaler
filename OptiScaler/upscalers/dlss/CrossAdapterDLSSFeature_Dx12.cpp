#include <pch.h>

#include "CrossAdapterDLSSFeature_Dx12.h"

#include <Config.h>
#include <misc/IdentifyGpu.h>
#include <proxies/D3D12_Proxy.h>
#include <proxies/Dxgi_Proxy.h>
#include <shaders/format_transfer/FT_Dx12.h>

#include <algorithm>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace
{
using PipelineClock = std::chrono::steady_clock;
constexpr DXGI_FORMAT PackedOutputFormat = DXGI_FORMAT_R11G11B10_FLOAT;

// DLSS-RR definitions were split into nvsdk_ngx_defs_dlssd.h in newer SDKs. OptiScaler currently ships the
// older combined NGX headers, so keep the ABI-stable parameter names local until that dependency is updated.
constexpr char DlssDenoiseMode[] = "DLSS.Denoise.Mode";
constexpr char DlssRoughnessMode[] = "DLSS.Roughness.Mode";
constexpr char DlssUseHwDepth[] = "DLSS.Use.HW.Depth";
constexpr char DlssInputDiffuseAlbedo[] = "DLSS.Input.DiffuseAlbedo";
constexpr char DlssInputSpecularAlbedo[] = "DLSS.Input.SpecularAlbedo";
constexpr char DlssInputReduceGhostMask[] = "DLSS.Input.Reduce.Ghost.Mask";
constexpr char DlssWorldToViewMatrix[] = "WorldToViewMatrix";
constexpr char DlssViewToClipMatrix[] = "ViewToClipMatrix";

template <typename T>
void CopyParameter(NVSDK_NGX_Parameter* source, NVSDK_NGX_Parameter* destination, const char* name)
{
    T value {};
    if (source->Get(name, &value) == NVSDK_NGX_Result_Success)
        destination->Set(name, value);
}

double ElapsedMilliseconds(PipelineClock::time_point start)
{
    return std::chrono::duration<double, std::milli>(PipelineClock::now() - start).count();
}

D3D12_RESOURCE_BARRIER MakeTransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

double SmoothTiming(double previous, double sample, bool firstSample)
{
    constexpr double smoothingFactor = 0.1;
    return firstSample ? sample : previous + (sample - previous) * smoothingFactor;
}

bool SupportsFormat(ID3D12Device* device, DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 requiredSupport1,
                    D3D12_FORMAT_SUPPORT2 requiredSupport2)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
    if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return false;

    const auto support1 = static_cast<UINT>(support.Support1);
    const auto support2 = static_cast<UINT>(support.Support2);
    const auto required1 = static_cast<UINT>(requiredSupport1);
    const auto required2 = static_cast<UINT>(requiredSupport2);
    return (support1 & required1) == required1 && (support2 & required2) == required2;
}

} // namespace

void CrossAdapterNGXFeatureDx12::TransferResource::Reset()
{
    if (renderMappedData && renderBuffer)
    {
        const D3D12_RANGE writtenRange { 0, output ? static_cast<SIZE_T>(renderTotalSize) : 0 };
        renderBuffer->Unmap(0, &writtenRange);
    }
    if (nvidiaMappedData && nvidiaBuffer)
    {
        const D3D12_RANGE writtenRange { 0, output ? 0 : static_cast<SIZE_T>(nvidiaTotalSize) };
        nvidiaBuffer->Unmap(0, &writtenRange);
    }

    renderMappedData = nullptr;
    nvidiaMappedData = nullptr;
    nvidiaTexture.Reset();
    nvidiaBuffer.Reset();
    renderBuffer.Reset();
    sourceDesc = {};
    bridgeDesc = {};
    renderFootprint = {};
    nvidiaFootprint = {};
    renderRowCount = 0;
    nvidiaRowCount = 0;
    renderRowSize = 0;
    nvidiaRowSize = 0;
    renderTotalSize = 0;
    nvidiaTotalSize = 0;
    nvidiaTextureState = D3D12_RESOURCE_STATE_COPY_DEST;
}

void CrossAdapterNGXFeatureDx12::AddTransfer(const char* parameter, const char* name, bool required, bool output,
                                             bool packedOutput)
{
    auto transfer = std::make_unique<TransferResource>();
    transfer->parameter = parameter;
    transfer->name = name;
    transfer->required = required;
    transfer->output = output;
    transfer->packedOutput = packedOutput;
    _transfers.push_back(transfer.get());
    _ownedTransfers.push_back(std::move(transfer));
}

void CrossAdapterNGXFeatureDx12::ConfigureTransfers()
{
    AddTransfer(NVSDK_NGX_Parameter_Color, "Color", true);
    AddTransfer(NVSDK_NGX_Parameter_MotionVectors, "MotionVectors", true);
    AddTransfer(NVSDK_NGX_Parameter_Depth, "Depth", true);
    AddTransfer(NVSDK_NGX_Parameter_ExposureTexture, "Exposure");
    AddTransfer(NVSDK_NGX_Parameter_TransparencyMask, "TransparencyMask");
    AddTransfer(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, "ReactiveMask");

    if (_featureKind == CrossAdapterNGXFeature::RayReconstruction)
    {
        AddTransfer(DlssInputDiffuseAlbedo, "DiffuseAlbedo", true);
        AddTransfer(DlssInputSpecularAlbedo, "SpecularAlbedo", true);
        AddTransfer(DlssInputReduceGhostMask, "ReduceGhostMask");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Normals, "Normals", true);
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Roughness, "Roughness");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Albedo, "GBufferAlbedo");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo, "GBufferDiffuseAlbedo");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_SpecularAlbedo, "GBufferSpecularAlbedo");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_IndirectAlbedo, "GBufferIndirectAlbedo");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_DisocclusionMask, "GBufferDisocclusionMask");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Metallic, "Metallic");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Specular, "Specular");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Subsurface, "Subsurface");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, "ShadingModelId");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_MaterialId, "MaterialId");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_8, "GBufferAttrib8");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_9, "GBufferAttrib9");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_10, "GBufferAttrib10");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_11, "GBufferAttrib11");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_12, "GBufferAttrib12");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_13, "GBufferAttrib13");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_14, "GBufferAttrib14");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_Atrrib_15, "GBufferAttrib15");
        AddTransfer("GBuffer.Emissive", "Emissive");
        AddTransfer(NVSDK_NGX_Parameter_MotionVectors3D, "MotionVectors3D");
        AddTransfer(NVSDK_NGX_Parameter_IsParticleMask, "ParticleMask");
        AddTransfer(NVSDK_NGX_Parameter_AnimatedTextureMask, "AnimatedTextureMask");
        AddTransfer(NVSDK_NGX_Parameter_DepthHighRes, "DepthHighRes");
        AddTransfer(NVSDK_NGX_Parameter_Position_ViewSpace, "PositionViewSpace");
        AddTransfer(NVSDK_NGX_Parameter_RayTracingHitDistance, "RayTracingHitDistance");
        AddTransfer(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, "SpecularMotionVectors");
        AddTransfer(NVSDK_NGX_Parameter_MotionVectorsReflection, "ReflectionMotionVectors");
        AddTransfer("DLSSD.ReflectedAlbedo", "ReflectedAlbedo");
        AddTransfer("DLSSD.ColorBeforeParticles", "ColorBeforeParticles");
        AddTransfer("DLSSD.ColorAfterParticles", "ColorAfterParticles");
        AddTransfer("DLSSD.ColorBeforeTransparency", "ColorBeforeTransparency");
        AddTransfer("DLSSD.ColorAfterTransparency", "ColorAfterTransparency");
        AddTransfer("DLSSD.ColorBeforeFog", "ColorBeforeFog");
        AddTransfer("DLSSD.ColorAfterFog", "ColorAfterFog");
        AddTransfer("DLSSD.ScreenSpaceSubsurfaceScatteringGuide", "SubsurfaceScatteringGuide");
        AddTransfer("DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering", "ColorBeforeSubsurfaceScattering");
        AddTransfer("DLSSD.ColorAfterScreenSpaceSubsurfaceScattering", "ColorAfterSubsurfaceScattering");
        AddTransfer("DLSSD.ScreenSpaceRefractionGuide", "ScreenSpaceRefractionGuide");
        AddTransfer("DLSSD.ColorBeforeScreenSpaceRefraction", "ColorBeforeScreenSpaceRefraction");
        AddTransfer("DLSSD.ColorAfterScreenSpaceRefraction", "ColorAfterScreenSpaceRefraction");
        AddTransfer("DLSSD.DepthOfFieldGuide", "DepthOfFieldGuide");
        AddTransfer("DLSSD.ColorBeforeDepthOfField", "ColorBeforeDepthOfField");
        AddTransfer("DLSSD.ColorAfterDepthOfField", "ColorAfterDepthOfField");
        AddTransfer("DLSSD.DiffuseHitDistance", "DiffuseHitDistance");
        AddTransfer("DLSSD.SpecularHitDistance", "SpecularHitDistance");
        AddTransfer("DLSSD.DiffuseRayDirection", "DiffuseRayDirection");
        AddTransfer("DLSSD.SpecularRayDirection", "SpecularRayDirection");
        AddTransfer("DLSSD.DiffuseRayDirectionHitDistance", "DiffuseRayDirectionHitDistance");
        AddTransfer("DLSSD.SpecularRayDirectionHitDistance", "SpecularRayDirectionHitDistance");
        AddTransfer(NVSDK_NGX_Parameter_DLSS_TransparencyLayer, "TransparencyLayer");
        AddTransfer(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity, "TransparencyLayerOpacity");
        AddTransfer(NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs, "TransparencyLayerMotionVectors");
        AddTransfer(NVSDK_NGX_Parameter_DLSS_DisocclusionMask, "DisocclusionMask");
        AddTransfer("DLSSD.ResponsivityMask", "ResponsivityMask");
        AddTransfer("DLSSD.Alpha", "Alpha");
        AddTransfer("DLSSD.OutputAlpha", "OutputAlpha", false, true);
    }

    AddTransfer(NVSDK_NGX_Parameter_Output, "Output", true, true, true);
    _output = _transfers.back();
}

CrossAdapterNGXFeatureDx12::CrossAdapterNGXFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters,
                                                       CrossAdapterNGXFeature featureKind)
    : IFeature(handleId, parameters), DLSSFeatureDx12(handleId, parameters), _featureKind(featureKind)
{
    _featureLabel = featureKind == CrossAdapterNGXFeature::RayReconstruction ? "DLSS Ray Reconstruction" : "DLSS";
    _nativeFeature = featureKind == CrossAdapterNGXFeature::RayReconstruction ? NVSDK_NGX_Feature_RayReconstruction
                                                                              : NVSDK_NGX_Feature_SuperSampling;
    _crossAdapterInfo.featureName = _featureLabel;
    ConfigureTransfers();
    LOG_WARN("EXPERIMENTAL cross-adapter {} requested (packed HDR output bridge)", _featureLabel);
}

CrossAdapterDLSSFeatureDx12::CrossAdapterDLSSFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters)
    : IFeature(handleId, parameters),
      CrossAdapterNGXFeatureDx12(handleId, parameters, CrossAdapterNGXFeature::SuperResolution)
{
}

CrossAdapterNGXFeatureDx12::~CrossAdapterNGXFeatureDx12()
{
    std::scoped_lock runtimeLock(_sharedRuntimeMutex);

    if (_capturePending && !State::Instance().isShuttingDown)
    {
        ID3D12CommandQueue* renderQueue = _renderQueue ? _renderQueue.Get() : State::Instance().currentCommandQueue;
        if (renderQueue)
            WaitForRenderQueue(renderQueue);
    }

    if (_nvidiaQueue && _nvidiaFence && _nvidiaFenceEvent)
    {
        const UINT64 value = ++_nvidiaFenceValue;
        if (SUCCEEDED(_nvidiaQueue->Signal(_nvidiaFence.Get(), value)))
            WaitForFence(_nvidiaFence.Get(), value, _nvidiaFenceEvent, "DLSS shutdown");
    }

    // The base destructor releases the feature after derived members have been destroyed, which is too late for
    // this class because the NGX feature belongs to _nvidiaDevice. Release it while that device is still alive.
    if (_pNativeHandle && NVNGXProxy::D3D12_ReleaseFeature())
    {
        NVNGXProxy::D3D12_ReleaseFeature()(_pNativeHandle);
        _pNativeHandle = nullptr;
    }

    if (_nvidiaParameters && NVNGXProxy::D3D12_DestroyParameters())
    {
        NVNGXProxy::D3D12_DestroyParameters()(_nvidiaParameters);
        _nvidiaParameters = nullptr;
    }

    ResetPackedOutputResources();
    _renderOutputUnpacker.reset();
    _nvidiaOutputPacker.reset();

    for (TransferResource* transfer : Transfers())
        transfer->Reset();

    // A quality change releases and immediately recreates the feature. NGX shutdown is a runtime-level operation;
    // invoking it here deadlocks the Streamline resize path under vkd3d-proton. Keep the secondary device and NGX
    // runtime alive so the replacement feature can be created against the same device.
    LOG_INFO("Cross-adapter {} released the feature; retaining the shared NVIDIA runtime for recreation",
             _featureLabel);

    if (_nvidiaFenceEvent)
        CloseHandle(_nvidiaFenceEvent);
    if (_renderFenceEvent)
        CloseHandle(_renderFenceEvent);
}

std::optional<CrossAdapterInfo> CrossAdapterNGXFeatureDx12::GetCrossAdapterInfo()
{
    std::scoped_lock statusLock(_statusMutex);
    return _crossAdapterInfo;
}

std::optional<double> CrossAdapterNGXFeatureDx12::ReadUpscalerTime(void* commandQueue)
{
    IFeature_Dx12::ReadUpscalerTime(commandQueue);

    const auto status = GetCrossAdapterInfo();
    std::optional<double> pipelineTime;
    if (status->completedFrames)
        pipelineTime = status->pipelineMs;
    return sumOpts(pipelineTime, lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void CrossAdapterNGXFeatureDx12::ReadDetailedGpuTimes(void* commandQueue,
                                                      std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    detailedGpuTimes.clear();

    const auto status = GetCrossAdapterInfo();
    if (status->completedFrames)
        detailedGpuTimes.emplace_back(DetailedGpuTime { "Cross-GPU CPU/NVIDIA pipeline", status->pipelineMs, true });

    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { "Render GPU bridge copies", lastUpscalerTime.value(), true });
    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });
    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(static_cast<ID3D12CommandQueue*>(commandQueue));
    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

void CrossAdapterNGXFeatureDx12::SetPipelineState(CrossAdapterPipelineState state)
{
    std::scoped_lock statusLock(_statusMutex);
    _crossAdapterInfo.state = state;
}

void CrossAdapterNGXFeatureDx12::CompleteFrame(const PipelineTiming& timing)
{
    std::scoped_lock statusLock(_statusMutex);
    const bool firstSample = _crossAdapterInfo.completedFrames == 0;
    _crossAdapterInfo.pipelineMs = SmoothTiming(_crossAdapterInfo.pipelineMs, timing.pipelineMs, firstSample);
    if (timing.hasNvidiaGpuTiming)
    {
        const bool firstGpuSample = !_crossAdapterInfo.nvidiaGpuTimingAvailable;
        const double transferRateMBs =
            timing.nvidiaTransferMs > 0.0 ? _transferredBytesPerFrame / timing.nvidiaTransferMs / 1000.0 : 0.0;
        _crossAdapterInfo.transferRateMBs =
            SmoothTiming(_crossAdapterInfo.transferRateMBs, transferRateMBs, firstGpuSample);
        _crossAdapterInfo.nvidiaFeatureMs =
            SmoothTiming(_crossAdapterInfo.nvidiaFeatureMs, timing.nvidiaFeatureMs, firstGpuSample);
        _crossAdapterInfo.nvidiaGpuTimingAvailable = true;
    }
    _crossAdapterInfo.state = CrossAdapterPipelineState::Active;
    ++_crossAdapterInfo.completedFrames;
}

bool CrossAdapterNGXFeatureDx12::CreateSecondaryDevice()
{
    auto gpus = IdentifyGpu::getAllGpus();
    const LUID renderLuid = Device->GetAdapterLuid();
    auto renderGpu = std::find_if(gpus.begin(), gpus.end(), [&renderLuid](const GpuInformation& gpu)
                                  { return IsEqualLUID(gpu.luid, renderLuid); });

    auto target = _sharedNvidiaDevice
                      ? std::find_if(gpus.begin(), gpus.end(), [](const GpuInformation& gpu)
                                     { return IsEqualLUID(gpu.luid, _sharedNvidiaDevice->GetAdapterLuid()); })
                      : std::find_if(gpus.begin(), gpus.end(), [](const GpuInformation& gpu)
                                     { return gpu.vendorId == VendorId::Nvidia && !gpu.softwareAdapter; });

    if (target == gpus.end())
    {
        LOG_ERROR("Cross-adapter DLSS could not find an NVIDIA adapter");
        return false;
    }

    {
        std::scoped_lock statusLock(_statusMutex);
        _crossAdapterInfo.renderGpuName = renderGpu != gpus.end() ? renderGpu->name : "Unknown render GPU";
        _crossAdapterInfo.upscalerGpuName = target->name;
    }

    if (IsEqualLUID(target->luid, renderLuid))
    {
        LOG_ERROR("Cross-adapter DLSS target is the render adapter; refusing to create a duplicate device");
        return false;
    }

    if (_sharedNvidiaDevice)
    {
        _nvidiaDevice = _sharedNvidiaDevice;
        LOG_INFO("Cross-adapter DLSS reusing the shared NVIDIA device");
        return true;
    }

    DxgiProxy::Init();
    D3d12Proxy::Init();
    if (!DxgiProxy::CreateDxgiFactory_() || !D3d12Proxy::D3D12CreateDevice_())
    {
        LOG_ERROR("Cross-adapter DLSS could not load the DXGI/D3D12 entry points");
        return false;
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(DxgiProxy::CreateDxgiFactory_()(__uuidof(factory), (IDXGIFactory**) factory.GetAddressOf())))
    {
        LOG_ERROR("Cross-adapter DLSS failed to create a DXGI factory");
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        ScopedSkipDxgiLoadChecks skipDxgiChecks {};
        if (FAILED(factory->EnumAdapterByLuid(target->luid, IID_PPV_ARGS(&adapter))))
        {
            LOG_ERROR("Cross-adapter DLSS failed to open NVIDIA adapter '{}'", target->name);
            return false;
        }
    }

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ScopedCreatingD3DDevice creatingDevice {};
        result = D3d12Proxy::D3D12CreateDevice_()(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_nvidiaDevice));
    }

    if (FAILED(result) || !_nvidiaDevice)
    {
        LOG_ERROR("Cross-adapter DLSS failed to create a D3D12 device on '{}': {:X}", target->name, (UINT) result);
        return false;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 {};
    _nvidiaDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    _nvidiaDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4));

    LOG_WARN("Cross-adapter DLSS target: '{}' (CrossNodeSharingTier {}, SharedResourceCompatibilityTier {})",
             target->name, (UINT) options.CrossNodeSharingTier, (UINT) options4.SharedResourceCompatibilityTier);
    _sharedNvidiaDevice = _nvidiaDevice;
    return true;
}

bool CrossAdapterNGXFeatureDx12::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    HRESULT result = _nvidiaDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_nvidiaQueue));
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS CreateCommandQueue failed: {:X}", (UINT) result);
        return false;
    }

    result = _nvidiaDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_nvidiaAllocator));
    if (FAILED(result))
        return false;

    result = _nvidiaDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _nvidiaAllocator.Get(), nullptr,
                                              IID_PPV_ARGS(&_nvidiaCommandList));
    if (FAILED(result))
        return false;

    result = _nvidiaDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_nvidiaFence));
    if (FAILED(result))
        return false;

    _nvidiaFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_nvidiaFenceEvent)
        return false;

    result = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_renderFence));
    if (FAILED(result))
        return false;

    _renderFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_renderFenceEvent)
        return false;

    CreateNvidiaGpuTimingResources();
    return true;
}

bool CrossAdapterNGXFeatureDx12::CreateNvidiaGpuTimingResources()
{
    HRESULT result = _nvidiaQueue->GetTimestampFrequency(&_nvidiaTimestampFrequency);
    if (FAILED(result) || !_nvidiaTimestampFrequency)
    {
        LOG_WARN("Cross-adapter DLSS NVIDIA GPU profiling disabled; timestamp frequency query failed: {:X}",
                 (UINT) result);
        return false;
    }

    D3D12_QUERY_HEAP_DESC queryHeapDesc {};
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryHeapDesc.Count = NvidiaTimestampCount;
    result = _nvidiaDevice->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&_nvidiaTimestampHeap));
    if (FAILED(result))
    {
        LOG_WARN("Cross-adapter DLSS NVIDIA GPU profiling disabled; query heap creation failed: {:X}", (UINT) result);
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = NvidiaTimestampCount * sizeof(UINT64);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    result = _nvidiaDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&_nvidiaTimestampReadback));
    if (FAILED(result))
    {
        LOG_WARN("Cross-adapter DLSS NVIDIA GPU profiling disabled; readback creation failed: {:X}", (UINT) result);
        _nvidiaTimestampHeap.Reset();
        return false;
    }

    _nvidiaGpuTimingEnabled = true;
    LOG_INFO("Cross-adapter DLSS NVIDIA GPU profiling enabled at {} Hz", _nvidiaTimestampFrequency);
    return true;
}

void CrossAdapterNGXFeatureDx12::RecordNvidiaTimestamp(NvidiaTimestamp timestamp)
{
    if (_nvidiaGpuTimingEnabled)
        _nvidiaCommandList->EndQuery(_nvidiaTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamp);
}

void CrossAdapterNGXFeatureDx12::ResolveNvidiaTimestamps()
{
    if (_nvidiaGpuTimingEnabled)
    {
        _nvidiaCommandList->ResolveQueryData(_nvidiaTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                             NvidiaTimestampCount, _nvidiaTimestampReadback.Get(), 0);
    }
}

void CrossAdapterNGXFeatureDx12::ReadNvidiaGpuTiming(PipelineTiming& timing)
{
    if (!_nvidiaGpuTimingEnabled)
        return;

    const D3D12_RANGE readRange { 0, NvidiaTimestampCount * sizeof(UINT64) };
    UINT64* mappedTimestamps = nullptr;
    const HRESULT result = _nvidiaTimestampReadback->Map(0, &readRange, reinterpret_cast<void**>(&mappedTimestamps));
    if (FAILED(result) || !mappedTimestamps)
    {
        LOG_WARN("Cross-adapter DLSS NVIDIA GPU profiling disabled; timestamp readback failed: {:X}", (UINT) result);
        _nvidiaGpuTimingEnabled = false;
        return;
    }

    std::array<UINT64, NvidiaTimestampCount> timestamps {};
    memcpy(timestamps.data(), mappedTimestamps, sizeof(timestamps));
    const D3D12_RANGE writtenRange { 0, 0 };
    _nvidiaTimestampReadback->Unmap(0, &writtenRange);

    if (timestamps[UploadEnd] < timestamps[UploadStart] || timestamps[FeatureEnd] < timestamps[UploadEnd] ||
        timestamps[PackEnd] < timestamps[FeatureEnd] || timestamps[ReadbackEnd] < timestamps[PackEnd])
    {
        LOG_WARN("Cross-adapter DLSS ignored invalid NVIDIA GPU timestamp ordering");
        return;
    }

    const double millisecondsPerTick = 1000.0 / static_cast<double>(_nvidiaTimestampFrequency);
    const double uploadMs = (timestamps[UploadEnd] - timestamps[UploadStart]) * millisecondsPerTick;
    timing.nvidiaFeatureMs = (timestamps[FeatureEnd] - timestamps[UploadEnd]) * millisecondsPerTick;
    const double readbackMs = (timestamps[ReadbackEnd] - timestamps[PackEnd]) * millisecondsPerTick;
    timing.nvidiaTransferMs = uploadMs + readbackMs;
    timing.hasNvidiaGpuTiming = true;
}

bool CrossAdapterNGXFeatureDx12::InitInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters)
{
    if (IsInited())
        return true;

    std::scoped_lock runtimeLock(_sharedRuntimeMutex);

    if (!Device || !CreateSecondaryDevice() || !CreateCommandObjects())
    {
        SetPipelineState(CrossAdapterPipelineState::Error);
        return false;
    }

    if (_sharedNgxInitialized)
    {
        NVNGXProxy::SetDx12Inited(true);
        _dlssInited = true;
        LOG_INFO("Cross-adapter {} reusing the initialized NVIDIA NGX runtime", _featureLabel);
    }
    else
    {
        // An initialization attempt made against the AMD render device must not suppress initialization on NVIDIA.
        NVNGXProxy::SetDx12Inited(false);
        _dlssInited = false;
    }

    ID3D12Device* renderDevice = Device;
    Device = _nvidiaDevice.Get();

    bool initialized = EnsureNGXInitialized();
    if (initialized)
        _sharedNgxInitialized = true;

    if (initialized && NVNGXProxy::D3D12_AllocateParameters())
    {
        const NVSDK_NGX_Result result = NVNGXProxy::D3D12_AllocateParameters()(&_nvidiaParameters);
        initialized = result == NVSDK_NGX_Result_Success && _nvidiaParameters;
        if (!initialized)
            LOG_ERROR("Cross-adapter {} native parameter allocation failed: {:X}", _featureLabel, (UINT) result);
    }
    else if (initialized)
    {
        LOG_ERROR("Cross-adapter {} native parameter allocation is unavailable", _featureLabel);
        initialized = false;
    }

    if (initialized)
    {
        CopyCreateParameters(parameters, _nvidiaParameters);
        ProcessNativeInitParams(_nvidiaParameters);
        _pNativeHandle = &_nativeHandle;
        LOG_WARN("Cross-adapter {} isolated NGX from the render-device parameter table: {:X} -> {:X}", _featureLabel,
                 (UINT64) parameters, (UINT64) _nvidiaParameters);

        NVSDK_NGX_Result createResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};
            createResult = NVNGXProxy::D3D12_CreateFeature()(_nvidiaCommandList.Get(), _nativeFeature,
                                                             _nvidiaParameters, &_pNativeHandle);
        }
        initialized = createResult == NVSDK_NGX_Result_Success;
        if (!initialized)
            LOG_ERROR("Cross-adapter {} CreateFeature failed: {:X}", _featureLabel, (UINT) createResult);
        else
        {
            ReadNativeVersion();
            SetInit(true);
        }
    }

    Device = renderDevice;

    if (!initialized || !ExecuteNvidiaCommands())
    {
        LOG_ERROR("Cross-adapter {} failed to initialize NGX on the secondary adapter", _featureLabel);
        SetInit(false);
        SetPipelineState(CrossAdapterPipelineState::Error);
        return false;
    }

    SetPipelineState(CrossAdapterPipelineState::Priming);
    LOG_WARN("Cross-adapter {} initialized; evaluations use a one-frame CPU staging pipeline", _featureLabel);
    return true;
}

void CrossAdapterNGXFeatureDx12::ProcessNativeInitParams(NVSDK_NGX_Parameter* parameters)
{
    DLSSFeature::ProcessInitParams(parameters);
}

void CrossAdapterNGXFeatureDx12::ProcessNativeEvaluateParams(NVSDK_NGX_Parameter* parameters)
{
    DLSSFeature::ProcessEvaluateParams(parameters);
}

void CrossAdapterNGXFeatureDx12::ReadNativeVersion() { DLSSFeature::ReadVersion(); }

void CrossAdapterNGXFeatureDx12::CopyCreateParameters(NVSDK_NGX_Parameter* source,
                                                      NVSDK_NGX_Parameter* destination) const
{
    constexpr std::array<const char*, 5> integerParameters = {
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        NVSDK_NGX_Parameter_PerfQualityValue,
        NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
        NVSDK_NGX_Parameter_FreeMemOnReleaseFeature,
        DlssDenoiseMode,
    };
    for (const char* name : integerParameters)
        CopyParameter<int>(source, destination, name);

    constexpr std::array<const char*, 14> unsignedParameters = {
        NVSDK_NGX_Parameter_Width,
        NVSDK_NGX_Parameter_Height,
        NVSDK_NGX_Parameter_OutWidth,
        NVSDK_NGX_Parameter_OutHeight,
        NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width,
        NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height,
        NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width,
        NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
    };
    for (const char* name : unsignedParameters)
        CopyParameter<unsigned int>(source, destination, name);

    if (_featureKind == CrossAdapterNGXFeature::RayReconstruction)
    {
        constexpr std::array<const char*, 11> rayReconstructionParameters = {
            NVSDK_NGX_Parameter_CreationNodeMask,
            NVSDK_NGX_Parameter_VisibilityNodeMask,
            DlssRoughnessMode,
            DlssUseHwDepth,
            "DLSS.Use.Folded.Network",
            "RayReconstruction.Hint.Render.Preset.DLAA",
            "RayReconstruction.Hint.Render.Preset.UltraQuality",
            "RayReconstruction.Hint.Render.Preset.Quality",
            "RayReconstruction.Hint.Render.Preset.Balanced",
            "RayReconstruction.Hint.Render.Preset.Performance",
            "RayReconstruction.Hint.Render.Preset.UltraPerformance",
        };
        for (const char* name : rayReconstructionParameters)
            CopyParameter<unsigned int>(source, destination, name);
    }
}

void CrossAdapterNGXFeatureDx12::CopyEvaluateParameters(NVSDK_NGX_Parameter* source, NVSDK_NGX_Parameter* destination)
{
    constexpr std::array<const char*, 4> integerParameters = {
        NVSDK_NGX_Parameter_Reset,
        NVSDK_NGX_Parameter_DLSS_Checkerboard_Jitter_Hack,
        NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis,
        NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis,
    };
    for (const char* name : integerParameters)
        CopyParameter<int>(source, destination, name);

    constexpr std::array<const char*, 9> floatParameters = {
        NVSDK_NGX_Parameter_Sharpness,   NVSDK_NGX_Parameter_Jitter_Offset_X,   NVSDK_NGX_Parameter_Jitter_Offset_Y,
        NVSDK_NGX_Parameter_MV_Scale_X,  NVSDK_NGX_Parameter_MV_Scale_Y,        NVSDK_NGX_Parameter_MV_Offset_X,
        NVSDK_NGX_Parameter_MV_Offset_Y, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale,
    };
    for (const char* name : floatParameters)
        CopyParameter<float>(source, destination, name);
    CopyParameter<float>(source, destination, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec);

    constexpr std::array<const char*, 17> unsignedParameters = {
        NVSDK_NGX_Parameter_Width,
        NVSDK_NGX_Parameter_Height,
        NVSDK_NGX_Parameter_OutWidth,
        NVSDK_NGX_Parameter_OutHeight,
        NVSDK_NGX_Parameter_TonemapperType,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
    };
    for (const char* name : unsignedParameters)
        CopyParameter<unsigned int>(source, destination, name);

    CopyParameter<unsigned int>(source, destination, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
    CopyParameter<unsigned int>(source, destination, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);

    if (_featureKind != CrossAdapterNGXFeature::RayReconstruction)
        return;

    constexpr std::array<const char*, 58> rayReconstructionSubrectParameters = {
        "DLSS.Input.DiffuseAlbedo.Subrect.Base.X",
        "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y",
        "DLSS.Input.SpecularAlbedo.Subrect.Base.X",
        "DLSS.Input.SpecularAlbedo.Subrect.Base.Y",
        "DLSS.Input.Normals.Subrect.Base.X",
        "DLSS.Input.Normals.Subrect.Base.Y",
        "DLSS.Input.Roughness.Subrect.Base.X",
        "DLSS.Input.Roughness.Subrect.Base.Y",
        "DLSS.Input.Reduce.Ghost.Subrect.Base.X",
        "DLSS.Input.Reduce.Ghost.Subrect.Base.Y",
        "DLSSD.Alpha.Subrect.Base.X",
        "DLSSD.Alpha.Subrect.Base.Y",
        "DLSSD.OutputAlpha.Subrect.Base.X",
        "DLSSD.OutputAlpha.Subrect.Base.Y",
        "DLSSD.ReflectedAlbedo.Subrect.Base.X",
        "DLSSD.ReflectedAlbedo.Subrect.Base.Y",
        "DLSSD.ColorBeforeParticles.Subrect.Base.X",
        "DLSSD.ColorBeforeParticles.Subrect.Base.Y",
        "DLSSD.ColorAfterParticles.Subrect.Base.X",
        "DLSSD.ColorAfterParticles.Subrect.Base.Y",
        "DLSSD.ColorBeforeTransparency.Subrect.Base.X",
        "DLSSD.ColorBeforeTransparency.Subrect.Base.Y",
        "DLSSD.ColorAfterTransparency.Subrect.Base.X",
        "DLSSD.ColorAfterTransparency.Subrect.Base.Y",
        "DLSSD.ColorBeforeFog.Subrect.Base.X",
        "DLSSD.ColorBeforeFog.Subrect.Base.Y",
        "DLSSD.ColorAfterFog.Subrect.Base.X",
        "DLSSD.ColorAfterFog.Subrect.Base.Y",
        "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.X",
        "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.Y",
        "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.X",
        "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.Y",
        "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.X",
        "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.Y",
        "DLSSD.ScreenSpaceRefractionGuide.Subrect.Base.X",
        "DLSSD.ScreenSpaceRefractionGuide.Subrect.Base.Y",
        "DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.X",
        "DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.Y",
        "DLSSD.ColorAfterScreenSpaceRefraction.Subrect.Base.X",
        "DLSSD.ColorAfterScreenSpaceRefraction.Subrect.Base.Y",
        "DLSSD.DepthOfFieldGuide.Subrect.Base.X",
        "DLSSD.DepthOfFieldGuide.Subrect.Base.Y",
        "DLSSD.ColorBeforeDepthOfField.Subrect.Base.X",
        "DLSSD.ColorBeforeDepthOfField.Subrect.Base.Y",
        "DLSSD.ColorAfterDepthOfField.Subrect.Base.X",
        "DLSSD.ColorAfterDepthOfField.Subrect.Base.Y",
        "DLSSD.DiffuseHitDistance.Subrect.Base.X",
        "DLSSD.DiffuseHitDistance.Subrect.Base.Y",
        "DLSSD.SpecularHitDistance.Subrect.Base.X",
        "DLSSD.SpecularHitDistance.Subrect.Base.Y",
        "DLSSD.DiffuseRayDirection.Subrect.Base.X",
        "DLSSD.DiffuseRayDirection.Subrect.Base.Y",
        "DLSSD.SpecularRayDirection.Subrect.Base.X",
        "DLSSD.SpecularRayDirection.Subrect.Base.Y",
        "DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.X",
        "DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.Y",
        "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.X",
        "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.Y",
    };
    for (const char* name : rayReconstructionSubrectParameters)
        CopyParameter<unsigned int>(source, destination, name);

    constexpr std::array<const char*, 10> auxiliarySubrectParameters = {
        NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y,
        "DLSSD.ResponsivityMask.Subrect.Base.X",
        "DLSSD.ResponsivityMask.Subrect.Base.Y",
    };
    for (const char* name : auxiliarySubrectParameters)
        CopyParameter<unsigned int>(source, destination, name);

    const auto copyMatrix = [source, destination](const char* name, std::array<float, 16>& storage)
    {
        void* matrix = nullptr;
        if (source->Get(name, &matrix) == NVSDK_NGX_Result_Success && matrix)
        {
            memcpy(storage.data(), matrix, sizeof(storage));
            destination->Set(name, static_cast<void*>(storage.data()));
        }
        else
        {
            destination->Set(name, static_cast<void*>(nullptr));
        }
    };
    copyMatrix(DlssWorldToViewMatrix, _worldToViewMatrix);
    copyMatrix(DlssViewToClipMatrix, _viewToClipMatrix);
}

bool CrossAdapterNGXFeatureDx12::GetResource(NVSDK_NGX_Parameter* parameters, const char* key,
                                             ID3D12Resource** resource)
{
    *resource = nullptr;
    auto result = parameters->Get(key, resource);
    if (result != NVSDK_NGX_Result_Success)
        result = parameters->Get(key, reinterpret_cast<void**>(resource));
    return result == NVSDK_NGX_Result_Success && *resource;
}

bool CrossAdapterNGXFeatureDx12::SameResourceDescription(const D3D12_RESOURCE_DESC& left,
                                                         const D3D12_RESOURCE_DESC& right)
{
    return left.Dimension == right.Dimension && left.Alignment == right.Alignment && left.Width == right.Width &&
           left.Height == right.Height && left.DepthOrArraySize == right.DepthOrArraySize &&
           left.MipLevels == right.MipLevels && left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count && left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Layout == right.Layout && left.Flags == right.Flags;
}

D3D12_RESOURCE_STATES CrossAdapterNGXFeatureDx12::GetRenderResourceState(const TransferResource& transfer)
{
    const auto& cfg = *Config::Instance();
    const auto configuredState = [&]() -> std::optional<int32_t>
    {
        if (!strcmp(transfer.parameter, NVSDK_NGX_Parameter_Color))
            return cfg.ColorResourceBarrier;
        if (!strcmp(transfer.parameter, NVSDK_NGX_Parameter_MotionVectors))
            return cfg.MVResourceBarrier;
        if (!strcmp(transfer.parameter, NVSDK_NGX_Parameter_Depth))
            return cfg.DepthResourceBarrier;
        if (!strcmp(transfer.parameter, NVSDK_NGX_Parameter_ExposureTexture))
            return cfg.ExposureResourceBarrier;
        if (!strcmp(transfer.parameter, NVSDK_NGX_Parameter_Output))
            return cfg.OutputResourceBarrier;
        return std::nullopt;
    }();

    if (configuredState.has_value())
        return static_cast<D3D12_RESOURCE_STATES>(configuredState.value());

    return transfer.output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

void CrossAdapterNGXFeatureDx12::Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!resource || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList->ResourceBarrier(1, &barrier);
}

void CrossAdapterNGXFeatureDx12::ResetPackedOutputResources()
{
    _nvidiaPackedOutput.Reset();
    _renderPackedOutput.Reset();
    _nvidiaPackedOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _renderPackedOutputState = D3D12_RESOURCE_STATE_COPY_DEST;
}

bool CrossAdapterNGXFeatureDx12::CreatePackedOutputResources(const D3D12_RESOURCE_DESC& sourceDesc)
{
    if (sourceDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        !(sourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    {
        LOG_ERROR("Cross-adapter DLSS requires an unordered-access R16G16B16A16_FLOAT output (format: {}, flags: "
                  "{:X})",
                  (UINT) sourceDesc.Format, (UINT) sourceDesc.Flags);
        return false;
    }

    const bool nvidiaSupported = SupportsFormat(_nvidiaDevice.Get(), sourceDesc.Format,
                                                D3D12_FORMAT_SUPPORT1_SHADER_LOAD, D3D12_FORMAT_SUPPORT2_NONE) &&
                                 SupportsFormat(_nvidiaDevice.Get(), PackedOutputFormat,
                                                D3D12_FORMAT_SUPPORT1_TEXTURE2D, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    const bool renderSupported =
        SupportsFormat(Device, PackedOutputFormat, D3D12_FORMAT_SUPPORT1_SHADER_LOAD, D3D12_FORMAT_SUPPORT2_NONE) &&
        SupportsFormat(Device, sourceDesc.Format, D3D12_FORMAT_SUPPORT1_TEXTURE2D,
                       D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    if (!nvidiaSupported || !renderSupported)
    {
        LOG_ERROR("Cross-adapter DLSS requires R11G11B10 output packing support (NVIDIA: {}, render GPU: {})",
                  nvidiaSupported, renderSupported);
        return false;
    }

    if (!_nvidiaOutputPacker)
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        _nvidiaOutputPacker =
            std::make_unique<FT_Dx12>("Cross-GPU output pack", _nvidiaDevice.Get(), PackedOutputFormat);
    }
    if (!_renderOutputUnpacker)
        _renderOutputUnpacker = std::make_unique<FT_Dx12>("Cross-GPU output unpack", Device, sourceDesc.Format);
    if (!_nvidiaOutputPacker->IsInit() || !_renderOutputUnpacker->IsInit())
    {
        LOG_ERROR("Cross-adapter DLSS output packing shaders failed to initialize");
        return false;
    }

    D3D12_RESOURCE_DESC packedDesc = sourceDesc;
    packedDesc.Format = PackedOutputFormat;
    packedDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProperties {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    HRESULT result =
        _nvidiaDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &packedDesc,
                                               _nvidiaPackedOutputState, nullptr, IID_PPV_ARGS(&_nvidiaPackedOutput));
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS could not create the NVIDIA packed output: {:X}", (UINT) result);
        ResetPackedOutputResources();
        return false;
    }

    packedDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    result = Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &packedDesc,
                                             _renderPackedOutputState, nullptr, IID_PPV_ARGS(&_renderPackedOutput));
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS could not create the render-GPU packed output: {:X}", (UINT) result);
        ResetPackedOutputResources();
        return false;
    }

    _nvidiaPackedOutput->SetName(L"Cross-GPU packed output (NVIDIA)");
    _renderPackedOutput->SetName(L"Cross-GPU packed output (render GPU)");
    LOG_WARN("Cross-adapter DLSS output bridge packing active: R16G16B16A16_FLOAT -> R11G11B10_FLOAT");
    return true;
}

bool CrossAdapterNGXFeatureDx12::CreateTransferResource(TransferResource& transfer, ID3D12Resource* source)
{
    const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
    if (transfer.renderBuffer && SameResourceDescription(transfer.sourceDesc, sourceDesc))
        return true;

    if (transfer.packedOutput)
        ResetPackedOutputResources();
    transfer.Reset();

    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.DepthOrArraySize != 1 ||
        sourceDesc.MipLevels != 1 || sourceDesc.SampleDesc.Count != 1)
    {
        LOG_ERROR("Cross-adapter DLSS {} has an unsupported resource layout", transfer.name);
        return false;
    }

    D3D12_RESOURCE_DESC bridgeDesc = sourceDesc;
    if (transfer.packedOutput)
    {
        if (!CreatePackedOutputResources(sourceDesc))
            return false;
        bridgeDesc.Format = PackedOutputFormat;
    }

    Device->GetCopyableFootprints(&bridgeDesc, 0, 1, 0, &transfer.renderFootprint, &transfer.renderRowCount,
                                  &transfer.renderRowSize, &transfer.renderTotalSize);
    _nvidiaDevice->GetCopyableFootprints(&bridgeDesc, 0, 1, 0, &transfer.nvidiaFootprint, &transfer.nvidiaRowCount,
                                         &transfer.nvidiaRowSize, &transfer.nvidiaTotalSize);
    if (!transfer.renderTotalSize || !transfer.nvidiaTotalSize || transfer.renderRowCount != transfer.nvidiaRowCount ||
        transfer.renderRowSize != transfer.nvidiaRowSize)
    {
        LOG_ERROR("Cross-adapter DLSS {} has incompatible copy footprints between adapters", transfer.name);
        return false;
    }

    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES renderHeap {};
    renderHeap.Type = transfer.output ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
    renderHeap.CreationNodeMask = 1;
    renderHeap.VisibleNodeMask = 1;
    bufferDesc.Width = transfer.renderTotalSize;
    HRESULT result = Device->CreateCommittedResource(&renderHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                     transfer.output ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                     : D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&transfer.renderBuffer));
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS CPU staging buffer creation failed for {} on render GPU: {:X}", transfer.name,
                  (UINT) result);
        return false;
    }

    D3D12_HEAP_PROPERTIES nvidiaHeap {};
    nvidiaHeap.Type = transfer.output ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
    nvidiaHeap.CreationNodeMask = 1;
    nvidiaHeap.VisibleNodeMask = 1;
    bufferDesc.Width = transfer.nvidiaTotalSize;
    result = _nvidiaDevice->CreateCommittedResource(&nvidiaHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                    transfer.output ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                    : D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&transfer.nvidiaBuffer));
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS CPU staging buffer creation failed for {} on NVIDIA GPU: {:X}", transfer.name,
                  (UINT) result);
        return false;
    }

    D3D12_RESOURCE_DESC localDesc = sourceDesc;
    localDesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(localDesc.Flags & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                                            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                                            D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
    if (transfer.output)
        localDesc.Flags =
            static_cast<D3D12_RESOURCE_FLAGS>(localDesc.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_HEAP_PROPERTIES localHeap {};
    localHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    localHeap.CreationNodeMask = 1;
    localHeap.VisibleNodeMask = 1;

    result = _nvidiaDevice->CreateCommittedResource(&localHeap, D3D12_HEAP_FLAG_NONE, &localDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&transfer.nvidiaTexture));
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS local texture creation failed for {}: {:X}", transfer.name, (UINT) result);
        return false;
    }

    transfer.sourceDesc = sourceDesc;
    transfer.bridgeDesc = bridgeDesc;
    if (!MapTransferResource(transfer))
    {
        transfer.Reset();
        return false;
    }

    LOG_INFO("Cross-adapter DLSS CPU bridge {}: {}x{}, format {} via {}, {} -> {} KiB", transfer.name, sourceDesc.Width,
             sourceDesc.Height, (UINT) sourceDesc.Format, (UINT) bridgeDesc.Format, transfer.renderTotalSize / 1024,
             transfer.nvidiaTotalSize / 1024);
    return true;
}

bool CrossAdapterNGXFeatureDx12::MapTransferResource(TransferResource& transfer)
{
    const D3D12_RANGE renderReadRange { 0, transfer.output ? 0 : static_cast<SIZE_T>(transfer.renderTotalSize) };
    void* mappedData = nullptr;
    HRESULT result = transfer.renderBuffer->Map(0, &renderReadRange, &mappedData);
    if (FAILED(result) || !mappedData)
    {
        LOG_ERROR("Cross-adapter DLSS could not persistently map {} staging buffer on render GPU: {:X}", transfer.name,
                  (UINT) result);
        return false;
    }
    transfer.renderMappedData = static_cast<std::byte*>(mappedData);

    const D3D12_RANGE dlssReadRange { 0, transfer.output ? static_cast<SIZE_T>(transfer.nvidiaTotalSize) : 0 };
    mappedData = nullptr;
    result = transfer.nvidiaBuffer->Map(0, &dlssReadRange, &mappedData);
    if (FAILED(result) || !mappedData)
    {
        LOG_ERROR("Cross-adapter DLSS could not persistently map {} staging buffer on NVIDIA GPU: {:X}", transfer.name,
                  (UINT) result);
        return false;
    }
    transfer.nvidiaMappedData = static_cast<std::byte*>(mappedData);
    return true;
}

bool CrossAdapterNGXFeatureDx12::CopyCpuStagingBuffer(TransferResource& transfer, bool renderToNvidia)
{
    const auto& sourceFootprint = renderToNvidia ? transfer.renderFootprint : transfer.nvidiaFootprint;
    const auto& destinationFootprint = renderToNvidia ? transfer.nvidiaFootprint : transfer.renderFootprint;
    const UINT rowCount = renderToNvidia ? transfer.renderRowCount : transfer.nvidiaRowCount;
    const UINT64 rowSize = renderToNvidia ? transfer.renderRowSize : transfer.nvidiaRowSize;
    const auto* sourceBase = renderToNvidia ? transfer.renderMappedData : transfer.nvidiaMappedData;
    auto* destinationBase = renderToNvidia ? transfer.nvidiaMappedData : transfer.renderMappedData;
    if (!sourceBase || !destinationBase)
    {
        LOG_ERROR("Cross-adapter DLSS {} staging buffer is not mapped", transfer.name);
        return false;
    }

    const auto* sourceBytes = sourceBase + static_cast<SIZE_T>(sourceFootprint.Offset);
    auto* destinationBytes = destinationBase + static_cast<SIZE_T>(destinationFootprint.Offset);

    const SIZE_T sourcePitch = sourceFootprint.Footprint.RowPitch;
    const SIZE_T destinationPitch = destinationFootprint.Footprint.RowPitch;
    if (sourcePitch == destinationPitch && rowCount > 0)
    {
        const SIZE_T copySize = static_cast<SIZE_T>(rowCount - 1) * sourcePitch + static_cast<SIZE_T>(rowSize);
        memcpy(destinationBytes, sourceBytes, copySize);
        return true;
    }

    for (UINT row = 0; row < rowCount; ++row)
        memcpy(destinationBytes + static_cast<SIZE_T>(row) * destinationPitch,
               sourceBytes + static_cast<SIZE_T>(row) * sourcePitch, static_cast<SIZE_T>(rowSize));

    return true;
}

bool CrossAdapterNGXFeatureDx12::CollectFrameResources(NVSDK_NGX_Parameter* parameters)
{
    for (TransferResource* transfer : Transfers())
    {
        transfer->frameResource = nullptr;
        if (!GetResource(parameters, transfer->parameter, &transfer->frameResource) && transfer->required)
        {
            LOG_ERROR("Cross-adapter DLSS required input {} is missing", transfer->name);
            return false;
        }
    }

    return true;
}

bool CrossAdapterNGXFeatureDx12::PrepareTransferResources()
{
    for (TransferResource* transfer : Transfers())
    {
        if (!transfer->frameResource)
        {
            transfer->Reset();
            continue;
        }

        if (!CreateTransferResource(*transfer, transfer->frameResource))
            return false;
    }

    UpdateTransferInfo();
    return true;
}

void CrossAdapterNGXFeatureDx12::UpdateTransferInfo()
{
    _transferredBytesPerFrame = 0;
    for (TransferResource* transfer : Transfers())
    {
        if (!transfer->renderBuffer)
            continue;

        _transferredBytesPerFrame += transfer->renderRowSize * transfer->renderRowCount;
    }
}

bool CrossAdapterNGXFeatureDx12::WaitForFence(ID3D12Fence* fence, UINT64 value, HANDLE eventHandle,
                                              const char* owner) const
{
    if (fence->GetCompletedValue() >= value)
        return true;

    HRESULT result = fence->SetEventOnCompletion(value, eventHandle);
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS {} SetEventOnCompletion failed: {:X}", owner, (UINT) result);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(eventHandle, 10000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("Cross-adapter DLSS {} fence timed out: {}", owner, waitResult);
        return false;
    }

    return true;
}

bool CrossAdapterNGXFeatureDx12::ValidateRenderQueue(ID3D12CommandQueue* queue)
{
    if (_renderQueue.Get() == queue)
        return true;

    ComPtr<ID3D12Device> queueDevice;
    if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
        !IsEqualLUID(queueDevice->GetAdapterLuid(), Device->GetAdapterLuid()))
    {
        LOG_ERROR("Cross-adapter DLSS captured command queue does not belong to the render device");
        return false;
    }

    _renderQueue = queue;
    LOG_INFO("Cross-adapter DLSS validated a render-device command queue");
    return true;
}

bool CrossAdapterNGXFeatureDx12::WaitForRenderQueue(ID3D12CommandQueue* queue)
{
    const UINT64 value = ++_renderFenceValue;
    HRESULT result = queue->Signal(_renderFence.Get(), value);
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS render queue signal failed: {:X}", (UINT) result);
        return false;
    }

    return WaitForFence(_renderFence.Get(), value, _renderFenceEvent, "render queue");
}

bool CrossAdapterNGXFeatureDx12::ExecuteNvidiaCommands()
{
    HRESULT result = _nvidiaCommandList->Close();
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS closing NVIDIA command list failed: {:X}", (UINT) result);
        return false;
    }

    ID3D12CommandList* lists[] = { _nvidiaCommandList.Get() };
    _nvidiaQueue->ExecuteCommandLists(1, lists);

    const UINT64 value = ++_nvidiaFenceValue;
    result = _nvidiaQueue->Signal(_nvidiaFence.Get(), value);
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS NVIDIA queue signal failed: {:X}", (UINT) result);
        return false;
    }

    if (!WaitForFence(_nvidiaFence.Get(), value, _nvidiaFenceEvent, "NVIDIA queue"))
        return false;

    result = _nvidiaAllocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("Cross-adapter DLSS NVIDIA allocator reset failed: {:X}", (UINT) result);
        return false;
    }

    result = _nvidiaCommandList->Reset(_nvidiaAllocator.Get(), nullptr);
    if (FAILED(result))
        LOG_ERROR("Cross-adapter DLSS NVIDIA command list reset failed: {:X}", (UINT) result);
    return SUCCEEDED(result);
}

bool CrossAdapterNGXFeatureDx12::CaptureInputs(ID3D12GraphicsCommandList* commandList)
{
    std::vector<D3D12_RESOURCE_BARRIER> prepareBarriers;
    std::vector<D3D12_RESOURCE_BARRIER> restoreBarriers;
    std::vector<ID3D12Resource*> transitionedResources;
    prepareBarriers.reserve(Transfers().size());
    restoreBarriers.reserve(Transfers().size());
    transitionedResources.reserve(Transfers().size());

    for (TransferResource* transfer : Transfers())
    {
        if (transfer->output || !transfer->renderBuffer)
            continue;

        if (std::find(transitionedResources.begin(), transitionedResources.end(), transfer->frameResource) !=
            transitionedResources.end())
            continue;

        transitionedResources.push_back(transfer->frameResource);
        const auto sourceState = GetRenderResourceState(*transfer);
        if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            prepareBarriers.push_back(
                MakeTransitionBarrier(transfer->frameResource, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE));
            restoreBarriers.push_back(
                MakeTransitionBarrier(transfer->frameResource, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState));
        }
    }

    if (!prepareBarriers.empty())
        commandList->ResourceBarrier(static_cast<UINT>(prepareBarriers.size()), prepareBarriers.data());

    for (TransferResource* transfer : Transfers())
    {
        if (transfer->output || !transfer->renderBuffer)
            continue;

        D3D12_TEXTURE_COPY_LOCATION destination {};
        destination.pResource = transfer->renderBuffer.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = transfer->renderFootprint;

        D3D12_TEXTURE_COPY_LOCATION sourceLocation {};
        sourceLocation.pResource = transfer->frameResource;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sourceLocation.SubresourceIndex = 0;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
    }

    if (!restoreBarriers.empty())
        commandList->ResourceBarrier(static_cast<UINT>(restoreBarriers.size()), restoreBarriers.data());

    return true;
}

bool CrossAdapterNGXFeatureDx12::CopyInputsToNvidia()
{
    for (TransferResource* transfer : Transfers())
    {
        if (transfer->output || !transfer->nvidiaBuffer)
            continue;

        if (!CopyCpuStagingBuffer(*transfer, true))
            return false;
    }

    std::vector<D3D12_RESOURCE_BARRIER> prepareBarriers;
    prepareBarriers.reserve(Transfers().size());
    for (TransferResource* transfer : Transfers())
    {
        if (!transfer->nvidiaBuffer)
            continue;

        const auto targetState =
            transfer->output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
        if (transfer->nvidiaTextureState != targetState)
        {
            prepareBarriers.push_back(
                MakeTransitionBarrier(transfer->nvidiaTexture.Get(), transfer->nvidiaTextureState, targetState));
            transfer->nvidiaTextureState = targetState;
        }
    }
    if (!prepareBarriers.empty())
        _nvidiaCommandList->ResourceBarrier(static_cast<UINT>(prepareBarriers.size()), prepareBarriers.data());

    std::vector<D3D12_RESOURCE_BARRIER> restoreBarriers;
    restoreBarriers.reserve(Transfers().size());
    for (TransferResource* transfer : Transfers())
    {
        if (transfer->output || !transfer->nvidiaBuffer)
            continue;

        D3D12_TEXTURE_COPY_LOCATION destination {};
        destination.pResource = transfer->nvidiaTexture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = transfer->nvidiaBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = transfer->nvidiaFootprint;
        _nvidiaCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        restoreBarriers.push_back(MakeTransitionBarrier(transfer->nvidiaTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        transfer->nvidiaTextureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (!restoreBarriers.empty())
        _nvidiaCommandList->ResourceBarrier(static_cast<UINT>(restoreBarriers.size()), restoreBarriers.data());

    return true;
}

bool CrossAdapterNGXFeatureDx12::EvaluateNvidia(PipelineTiming& timing)
{
    if (!_nvidiaParameters)
        return false;

    for (TransferResource* transfer : Transfers())
    {
        ID3D12Resource* resource = transfer->nvidiaTexture.Get();
        _nvidiaParameters->Set(transfer->parameter, resource);
    }

    ProcessNativeEvaluateParams(_nvidiaParameters);
    const NVSDK_NGX_Result evaluateResult =
        NVNGXProxy::D3D12_EvaluateFeature()(_nvidiaCommandList.Get(), _pNativeHandle, _nvidiaParameters, nullptr);
    if (evaluateResult != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("Cross-adapter {} EvaluateFeature failed: {:X}", _featureLabel, (UINT) evaluateResult);
        return false;
    }
    ++_frameCount;

    RecordNvidiaTimestamp(FeatureEnd);

    Transition(_nvidiaCommandList.Get(), _output->nvidiaTexture.Get(), _output->nvidiaTextureState,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _output->nvidiaTextureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(_nvidiaCommandList.Get(), _nvidiaPackedOutput.Get(), _nvidiaPackedOutputState,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    _nvidiaPackedOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (!_nvidiaOutputPacker->Dispatch(_nvidiaCommandList.Get(), _output->nvidiaTexture.Get(),
                                       _nvidiaPackedOutput.Get()))
    {
        LOG_ERROR("Cross-adapter DLSS output packing dispatch failed");
        return false;
    }

    Transition(_nvidiaCommandList.Get(), _output->nvidiaTexture.Get(), _output->nvidiaTextureState,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    _output->nvidiaTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Transition(_nvidiaCommandList.Get(), _nvidiaPackedOutput.Get(), _nvidiaPackedOutputState,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    _nvidiaPackedOutputState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    RecordNvidiaTimestamp(PackEnd);

    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = _output->nvidiaBuffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = _output->nvidiaFootprint;

    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = _nvidiaPackedOutput.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    _nvidiaCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    Transition(_nvidiaCommandList.Get(), _nvidiaPackedOutput.Get(), _nvidiaPackedOutputState,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    _nvidiaPackedOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (!ReadBackOutputs())
        return false;
    RecordNvidiaTimestamp(ReadbackEnd);
    ResolveNvidiaTimestamps();
    if (!ExecuteNvidiaCommands())
        return false;

    ReadNvidiaGpuTiming(timing);
    return true;
}

bool CrossAdapterNGXFeatureDx12::ReadBackOutputs()
{
    for (TransferResource* transfer : Transfers())
    {
        if (!transfer->output || transfer->packedOutput || !transfer->nvidiaBuffer)
            continue;

        Transition(_nvidiaCommandList.Get(), transfer->nvidiaTexture.Get(), transfer->nvidiaTextureState,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transfer->nvidiaTextureState = D3D12_RESOURCE_STATE_COPY_SOURCE;

        D3D12_TEXTURE_COPY_LOCATION destination {};
        destination.pResource = transfer->nvidiaBuffer.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = transfer->nvidiaFootprint;

        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = transfer->nvidiaTexture.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        _nvidiaCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        Transition(_nvidiaCommandList.Get(), transfer->nvidiaTexture.Get(), transfer->nvidiaTextureState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transfer->nvidiaTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    return true;
}

bool CrossAdapterNGXFeatureDx12::InjectOutputs(ID3D12GraphicsCommandList* commandList)
{
    if (!_output->frameResource)
        return false;

    if (!CopyCpuStagingBuffer(*_output, false))
        return false;

    const auto outputState = GetRenderResourceState(*_output);
    Transition(commandList, _renderPackedOutput.Get(), _renderPackedOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
    _renderPackedOutputState = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_TEXTURE_COPY_LOCATION packedDestination {};
    packedDestination.pResource = _renderPackedOutput.Get();
    packedDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    packedDestination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION packedSource {};
    packedSource.pResource = _output->renderBuffer.Get();
    packedSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    packedSource.PlacedFootprint = _output->renderFootprint;
    commandList->CopyTextureRegion(&packedDestination, 0, 0, 0, &packedSource, nullptr);

    Transition(commandList, _renderPackedOutput.Get(), _renderPackedOutputState,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _renderPackedOutputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(commandList, _output->frameResource, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!_renderOutputUnpacker->Dispatch(commandList, _renderPackedOutput.Get(), _output->frameResource))
    {
        LOG_ERROR("Cross-adapter DLSS output unpacking dispatch failed");
        return false;
    }

    D3D12_RESOURCE_BARRIER uavBarrier {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _output->frameResource;
    commandList->ResourceBarrier(1, &uavBarrier);
    Transition(commandList, _output->frameResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputState);
    Transition(commandList, _renderPackedOutput.Get(), _renderPackedOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
    _renderPackedOutputState = D3D12_RESOURCE_STATE_COPY_DEST;

    for (TransferResource* transfer : Transfers())
    {
        if (!transfer->output || transfer->packedOutput || !transfer->frameResource)
            continue;
        if (!CopyCpuStagingBuffer(*transfer, false))
            return false;

        const auto outputState = GetRenderResourceState(*transfer);
        Transition(commandList, transfer->frameResource, outputState, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION destination {};
        destination.pResource = transfer->frameResource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = transfer->renderBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = transfer->renderFootprint;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(commandList, transfer->frameResource, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
    }
    return true;
}

bool CrossAdapterNGXFeatureDx12::OutputDescriptionsMatch() const
{
    if (!_output->frameResource || !_output->renderBuffer ||
        !SameResourceDescription(_output->sourceDesc, _output->frameResource->GetDesc()) || !_nvidiaOutputPacker ||
        !_renderOutputUnpacker || !_nvidiaPackedOutput || !_renderPackedOutput)
        return false;

    return std::all_of(Transfers().begin(), Transfers().end(),
                       [](const TransferResource* transfer)
                       {
                           return !transfer->output || !transfer->frameResource ||
                                  (transfer->renderBuffer &&
                                   SameResourceDescription(transfer->sourceDesc, transfer->frameResource->GetDesc()));
                       });
}

bool CrossAdapterNGXFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* commandList,
                                                  NVSDK_NGX_Parameter* parameters)
{
    std::scoped_lock pipelineLock(_pipelineMutex);
    const auto pipelineStart = PipelineClock::now();
    PipelineTiming timing;
    bool producedOutput = false;

    if (!commandList)
    {
        LOG_ERROR("Cross-adapter DLSS received a null game command list");
        SetPipelineState(CrossAdapterPipelineState::Error);
        return false;
    }

    ID3D12CommandQueue* renderQueue = State::Instance().currentCommandQueue;
    if (!renderQueue)
    {
        LOG_ERROR("Cross-adapter DLSS has not captured the game's D3D12 command queue yet");
        SetPipelineState(CrossAdapterPipelineState::Error);
        return false;
    }
    if (!ValidateRenderQueue(renderQueue))
    {
        SetPipelineState(CrossAdapterPipelineState::Error);
        return false;
    }
    renderQueue = _renderQueue.Get();

    if (!CollectFrameResources(parameters))
    {
        SetPipelineState(CrossAdapterPipelineState::Error);
        return false;
    }

    if (_capturePending && !OutputDescriptionsMatch())
    {
        // The staging resources may still be referenced by the preceding game command list. Drain it before
        // discarding the delayed frame and rebuilding resources for the new dimensions or format.
        if (!WaitForRenderQueue(renderQueue))
        {
            SetPipelineState(CrossAdapterPipelineState::Error);
            return false;
        }

        LOG_WARN("Cross-adapter DLSS output changed; discarding the pending frame and repriming the pipeline");
        _capturePending = false;
        SetPipelineState(CrossAdapterPipelineState::Priming);
    }

    if (_capturePending)
    {
        // The previous evaluation recorded its input copies into the game's command list. At the next evaluation,
        // that list has been submitted, so this fence makes its readback buffers safe for CPU access.
        if (!WaitForRenderQueue(renderQueue))
        {
            _capturePending = false;
            SetPipelineState(CrossAdapterPipelineState::Error);
            LOG_ERROR("Cross-adapter DLSS render-queue synchronization failed");
            return false;
        }

        RecordNvidiaTimestamp(UploadStart);
        if (!CopyInputsToNvidia())
        {
            _capturePending = false;
            SetPipelineState(CrossAdapterPipelineState::Error);
            LOG_ERROR("Cross-adapter DLSS input transfer failed");
            return false;
        }
        RecordNvidiaTimestamp(UploadEnd);

        if (!EvaluateNvidia(timing))
        {
            _capturePending = false;
            SetPipelineState(CrossAdapterPipelineState::Error);
            LOG_ERROR("Cross-adapter DLSS NVIDIA evaluation failed");
            return false;
        }

        if (!InjectOutputs(commandList))
        {
            _capturePending = false;
            SetPipelineState(CrossAdapterPipelineState::Error);
            LOG_ERROR("Cross-adapter DLSS output transfer failed");
            return false;
        }
        producedOutput = true;
    }
    else
    {
        SetPipelineState(CrossAdapterPipelineState::Priming);
        if (!_primingWarningLogged)
        {
            LOG_WARN("Cross-adapter DLSS is priming its one-frame CPU staging pipeline");
            _primingWarningLogged = true;
        }
    }

    if (!PrepareTransferResources())
    {
        _capturePending = false;
        SetPipelineState(CrossAdapterPipelineState::Error);
        LOG_ERROR("Cross-adapter DLSS transfer-resource preparation failed");
        return false;
    }

    if (!CaptureInputs(commandList))
    {
        _capturePending = false;
        SetPipelineState(CrossAdapterPipelineState::Error);
        LOG_ERROR("Cross-adapter DLSS input capture failed");
        return false;
    }

    // Resource pointers above are replaced with NVIDIA resources during delayed evaluation. Cache this frame's
    // scalar values now so they remain paired with the captured textures on the next call.
    CopyEvaluateParameters(parameters, _nvidiaParameters);
    _capturePending = true;

    if (producedOutput)
    {
        timing.pipelineMs = ElapsedMilliseconds(pipelineStart);
        CompleteFrame(timing);
    }

    return true;
}

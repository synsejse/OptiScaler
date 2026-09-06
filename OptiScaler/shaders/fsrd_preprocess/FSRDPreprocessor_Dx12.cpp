#include "pch.h"
#include "FSRDPreprocessor_Dx12.h"
#include "FSRDShaderUtils.h"
#include "FSRDShaderData.h"
#include <fsrd_generated/FSRDInputConv_Shader.h>
#include <fsrd_generated/FSRDFloorSeed_Shader.h>
#include <fsrd_generated/FSRDFloor_Shader.h>
#include <fsrd_generated/FSRDOutputComp_Shader.h>

#include "dx12/ffx_api_dx12.h"
#include "fsr-rr/ffx_denoiser.h"

#include <d3dcompiler.h>
#include <d3d12.h>
#include <stdexcept>
#include <vector>
#include <string>
#include <array>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace FSRD;

constexpr UINT kBackBufferCount = 3;

constexpr UINT kThreadGroupSizeX = 8;
constexpr UINT kThreadGroupSizeY = 8;

constexpr D3D12_RESOURCE_STATES kSrvState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

namespace FSRDFormats
{
    // ffxDispatchDescDenoiserInput1Signal
    constexpr DXGI_FORMAT Radiance = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT FusedAlbedo = DXGI_FORMAT_R10G10B10A2_UNORM;

    // ffxDispatchDescDenoiser
    constexpr DXGI_FORMAT Motion = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT Normals = DXGI_FORMAT_R10G10B10A2_UNORM;
    // Match AMD's sample: higher precision at the same four bytes/pixel. Alpha is unused in RR 1.1.
    constexpr DXGI_FORMAT SpecAlbedo = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr DXGI_FORMAT DiffAlbedo = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr DXGI_FORMAT LinearDepth = DXGI_FORMAT_R32_FLOAT;

    constexpr DXGI_FORMAT SkipSignal = DXGI_FORMAT_R16G16B16A16_FLOAT;

    constexpr DXGI_FORMAT OutputBuffer1 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT OutputBuffer2 = DXGI_FORMAT_R16G16B16A16_FLOAT;

    constexpr DXGI_FORMAT SmoothFloor = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT EdgeGuide = DXGI_FORMAT_R8_UNORM;
}

struct ComputeState
{
    ID3D12Device* m_pDev = nullptr;
    
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    std::vector<FrameDescriptorHeap> m_frameHeaps;

    ComPtr<ID3D12Resource> m_constUploadBuffer;
    byte* m_cbMappedData = nullptr;
    UINT m_cbSlotSize = 0;
    UINT m_cbCurrentFrameIndex = 0;
    UINT backBufferCount = kBackBufferCount;

    ~ComputeState()
    {
        if (m_constUploadBuffer && m_cbMappedData)
        {
            m_constUploadBuffer->Unmap(0, nullptr);
            m_cbMappedData = nullptr;
        }
    }

    void Initialize(
        ID3D12Device* pDev,
        std::span<const byte> bytecode,
        UINT cbDataSize,
        UINT numSrvs,
        UINT numUavs,
        LPCWSTR cbName,
        UINT backBufferCount = kBackBufferCount)
    {
        m_pDev = pDev;
        this->backBufferCount = backBufferCount;

        // Create Root Signature
        ThrowIfFailed(m_pDev->CreateRootSignature(0, bytecode.data(), bytecode.size(), IID_PPV_ARGS(&m_rootSig)),
              "Failed to create Root Signature");

        // Create PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS = { bytecode.data(), bytecode.size() };
        ThrowIfFailed(m_pDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)), "Failed to create PSO");

        // Create Constant Buffer Upload Heap
        m_cbSlotSize = AlignTo256(cbDataSize);
        const UINT bufferSize = m_cbSlotSize * backBufferCount;

        D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = bufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(m_pDev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, 
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constUploadBuffer)), "Failed to create Constant Buffer");
        
        m_constUploadBuffer->SetName(cbName);
        D3D12_RANGE readRange = { 0, 0 }; 
        ThrowIfFailed(m_constUploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_cbMappedData)), "Failed to map Constant Buffer");

        m_frameHeaps.resize(backBufferCount);

        // Create Descriptor Heaps
        for (auto& heap : m_frameHeaps)
        {
            if (!heap.Initialize(m_pDev, numSrvs, numUavs, 0, 0))
                throw std::runtime_error("Failed to initialize FrameDescriptorHeap");
        }
    }

    void Dispatch(
        ID3D12GraphicsCommandList* cmdList,
        std::span<const byte> cbData,
        std::span<ID3D12Resource* const> inputs,
        std::span<const MipChainDesc> inputMips,
        std::span<ID3D12Resource*> output,
        std::span<const UINT> outputMips,
        XMFLOAT2 outDim,
        bool autoBarrierOutput = true
    )
    {
        if (!cmdList) 
            return;

        ScopedSkipHeapCapture skipHeapCapture {};

        // Constant Buffer Updates
        const UINT currentFrame = m_cbCurrentFrameIndex;
        const UINT currentOffset = currentFrame * m_cbSlotSize;
        memcpy(m_cbMappedData + currentOffset, cbData.data(), cbData.size());

        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = m_constUploadBuffer->GetGPUVirtualAddress() + currentOffset;
        m_cbCurrentFrameIndex = (m_cbCurrentFrameIndex + 1) % backBufferCount;

        // Transitions SRV -> UAV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kSrvState, kUavState);

        // Update descriptors
        FrameDescriptorHeap& currentHeap = m_frameHeaps[currentFrame];
        CreateSRVs(m_pDev, currentHeap, inputs, inputMips);
        CreateUAVs(m_pDev, currentHeap, output, outputMips);

        // Configure pipeline
        cmdList->SetPipelineState(m_pso.Get());
        cmdList->SetComputeRootSignature(m_rootSig.Get());

        ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0, cbAddress);

        // SRV table
        cmdList->SetComputeRootDescriptorTable(1, currentHeap.GetTableGPUStart());

        // UAV table
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavTable = currentHeap.GetTableGPUStart();
        uavTable.Offset((UINT)inputs.size(), m_pDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        cmdList->SetComputeRootDescriptorTable(2, uavTable);

        // Dispatch
        const UINT dimX = ((UINT)outDim.x + (kThreadGroupSizeX - 1)) / kThreadGroupSizeX;
        const UINT dimY = ((UINT)outDim.y + (kThreadGroupSizeY - 1)) / kThreadGroupSizeY;
        cmdList->Dispatch(dimX, dimY, 1);

        // Transition the UAVs back to SRV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kUavState, kSrvState);
    }

    void Dispatch(
        ID3D12GraphicsCommandList* cmdList,
        std::span<const byte> cbData,
        std::span<ID3D12Resource* const> inputs,
        std::span<ID3D12Resource*> output,
        XMFLOAT2 outDim,
        bool autoBarrierOutput = true
    )
    {
        Dispatch(cmdList, cbData, inputs, {}, output, {}, outDim, autoBarrierOutput);
    }
};

// Private implementation
struct FSRDPreprocessor_Dx12::Impl
{
    ID3D12Device* m_pDev = nullptr;

    ComputeState m_floorSeedShader;
    ComputeState m_floorFilterShader;
    ComputeState m_convShader;
    ComputeState m_compShader;

    UINT m_maxWidth = 0;
    UINT m_maxHeight = 0;
    XMFLOAT2 m_renderSize = {};

    // Output Targets
    // Internal storage
    Conversion::Output m_out;
    ComPtr<ID3D12Resource> m_outputBuffer1;
    ComPtr<ID3D12Resource> m_outputBuffer2;

    // Floor filter
    ID3D12Resource* m_smoothFloor;
    ComPtr<ID3D12Resource> m_edgeGuide;

    void Initialize(std::span<const byte> blSeedByteCode, std::span<const byte> blPyramidByteCode,
                    std::span<const byte> convByteCode, std::span<const byte> compByteCode)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        LOG_DEBUG("Creating FSRD interop shaders...");

        m_floorSeedShader.Initialize(m_pDev, blSeedByteCode, sizeof(FloorSeed::Constants), 
            FloorSeed::Input::kCount, FloorSeed::Output::kCount, L"FSRD_FloorSeed_Constants", FloorSeed::kBackBufferCount);
        m_floorFilterShader.Initialize(m_pDev, blPyramidByteCode, sizeof(FloorFilter::Constants), 
            FloorFilter::Input::kCount, FloorFilter::Output::kCount, L"FSRD_FloorFilter_Constants", FloorFilter::kBackBufferCount);
        m_convShader.Initialize(m_pDev, convByteCode, sizeof(Conversion::Constants), 
            Conversion::Input::kCount, Conversion::Output::kCount, L"FSRD_Conv_Constants", Conversion::kBackBufferCount);
        m_compShader.Initialize(m_pDev, compByteCode, sizeof(Composition::Constants), 
            Composition::Input::kCount, Composition::kOutputCount, L"FSRD_Comp_Constants", Composition::kBackBufferCount);

        LOG_DEBUG("FSRD interop shaders and resources initialized.");
    }

    void SetMaxRenderSize(UINT width, UINT height)
    {
        if (m_maxWidth == width && m_maxHeight == height)
            return;

        m_maxWidth = width;
        m_maxHeight = height;

        auto CreateTex = [&](DXGI_FORMAT fmt, LPCWSTR name, UINT mipLevels = 1)
        { 
            return CreateTexture2D(m_pDev, width, height, fmt, name, kSrvState, mipLevels);
        };

        auto& outResources = m_out.Resources;
        outResources.Motion = CreateTex(FSRDFormats::Motion, L"FSR_Conv_Motion");
        outResources.Normals = CreateTex(FSRDFormats::Normals, L"FSR_Conv_Normals");
        outResources.SpecAlbedo = CreateTex(FSRDFormats::SpecAlbedo, L"FSR_Conv_SpecAlbedo");
        outResources.DiffAlbedo = CreateTex(FSRDFormats::DiffAlbedo, L"FSR_Conv_DiffAlbedo");
        outResources.LinearDepth = CreateTex(FSRDFormats::LinearDepth, L"FSR_Conv_LinearDepth");
        outResources.SkipSignal = CreateTex(FSRDFormats::SkipSignal, L"FSR_Conv_SkipSignal");

        m_outputBuffer1 = CreateTex(FSRDFormats::OutputBuffer1, L"FSR_Conv_OutputBuffer1");
        // The floor filter still needs two scratch buffers; only buffer 1 is a denoiser output.
        m_outputBuffer2 = CreateTex(FSRDFormats::OutputBuffer2, L"FSR_Conv_OutputBuffer2");

        m_smoothFloor = nullptr;
        m_edgeGuide = CreateTex(FSRDFormats::EdgeGuide, L"FSR_Conv_EdgeGuide");

        outResources.Radiance = CreateTex(FSRDFormats::Radiance, L"FSR_Conv_Radiance");
        outResources.FusedAlbedo = CreateTex(FSRDFormats::FusedAlbedo, L"FSR_Conv_FusedAlbedo");
    }

    void DispatchPyramidSeed(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc) 
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };
        const bool isDepthLinear = (desc.Flags & (uint32_t) ConvFlags::IsDepthLinear);
        m_smoothFloor = m_outputBuffer1.Get();

        FloorSeed::Constants constants = 
        { 
            .InvProjMatrix = desc.InvProjMatrix,
            .RenderSize = desc.RenderSize,
            .NearPlane = desc.NearPlane,
            .FarPlane = desc.FarPlane,
            .Flags = isDepthLinear ? uint32_t(FloorSeed::Flags::LinearDepth) : 0u
        };
        const auto cbData = GetAsByteSpan(constants);

        // Create median filtered raw color before cross bilateral filtering
        // Write to mip chain at top level
        FloorSeed::Input in = { .Resources =  
        {
            .InColor = desc.Resources.InColor,
            .InSpecAlbedo = desc.Resources.InSpecAlbedo,
            .InDiffAlbedo = desc.Resources.InDiffAlbedo,
            .InDepth = desc.Resources.InDepth
        }};

        FloorSeed::Output out = { .Resources = 
        {
            .OutColor = m_smoothFloor,
            .OutEdges = m_edgeGuide.Get()
        }};

        m_floorSeedShader.Dispatch(cmdList, cbData, in.AsArray, out.AsArray, dispatchSize);
    }

    void DispatchFloorFilter(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc) 
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };

        for (int i = 0; i <= FloorFilter::kPasses; i++)
        {
            FloorFilter::Constants constants = 
            {
                .DstTexSize = desc.RenderSize,
                .StepSize = 1 << i
            };
            const auto cbData = GetAsByteSpan(constants);

            FloorFilter::Input in = { .Resources = 
            {
                .InColor = m_smoothFloor,
                .InEdgeGuide = m_edgeGuide.Get()
            }};

            FloorFilter::Output out = { .Resources = 
            {
                .OutColor = m_outputBuffer2.Get()
            }};

            m_floorFilterShader.Dispatch(cmdList, cbData, in.AsArray, out.AsArray, dispatchSize);

            std::swap(m_outputBuffer1, m_outputBuffer2);
            m_smoothFloor = m_outputBuffer1.Get();
        }
    }

    void DispatchPackingShader(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc) 
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };

        // Prepare inputs for packing and format conversion
        Conversion::Input in = {};
        memcpy_s(in.AsArray, sizeof(in.AsArray), desc.Resources.AsArray, sizeof(desc.Resources.AsArray));

        Conversion::Constants packConstants =
        {
            .InvViewMatrix = desc.InvViewMatrix,
            .InvProjMatrix = desc.InvProjMatrix,
            .PrevViewMatrix = desc.PrevViewMatrix,
            .RenderSize = desc.RenderSize,
            .NearPlane = desc.NearPlane,
            .FarPlane = desc.FarPlane,
            .FloorIsolation = desc.FloorIsolation,
            .Flags = desc.Flags
        };

        in.Resources.InBlurColor = m_smoothFloor;

        const std::span<const byte> convCBData((const byte*) &packConstants, sizeof(packConstants));
        auto outputs = m_out.GetRawResources();
        m_convShader.Dispatch(cmdList, convCBData, in.AsArray, outputs, dispatchSize, true);
    }

    void DispatchConversion(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc) 
    {
        if (!cmdList || !m_maxWidth)
            throw std::runtime_error("FSRD conversion has no command list or allocated buffers");
        if (desc.RenderSize.x <= 0 || desc.RenderSize.y <= 0 || desc.RenderSize.x > m_maxWidth ||
            desc.RenderSize.y > m_maxHeight)
            throw std::runtime_error("FSRD render size exceeds allocated buffers");
        m_renderSize = { desc.RenderSize.x, desc.RenderSize.y };

        // Filtered raster lighting estimate
        DispatchPyramidSeed(cmdList, desc);
        DispatchFloorFilter(cmdList, desc);

        // DLSS-RR to FSR-RR conversion
        DispatchPackingShader(cmdList, desc);

    }

    void DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc)
    {
        if (!cmdList || !m_maxWidth)
            return;

        auto& outResources = m_out.Resources;
        Composition::Input inputs = {};
        Composition::Constants constants = 
        {
            .DstTexSize = desc.DstTexSize,
            .CorrelationBias = desc.CorrelationBias,
            .Flags = UINT(desc.Flags) 
        };

        inputs.Resources = { .InDenoisedRadiance = m_outputBuffer1.Get(),
                             .InFusedAlbedo = outResources.FusedAlbedo.Get(),
                             .InSkipSignal = outResources.SkipSignal.Get(),
                             .InRawColor = desc.InRawColor,
                             .InColorBeforeParticles = desc.InColorBeforeParticles };

        std::array<ID3D12Resource*, 1> uavs { m_out.Resources.Motion.Get() };
        const std::span<const byte> cbData((const byte*) &constants, sizeof(constants));
        const XMFLOAT2 dstDim = { constants.DstTexSize.x, constants.DstTexSize.y };

        m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, uavs, dstDim, true);
    }

    void Blit(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* srcTex, ID3D12Resource* dstTex,
              XMFLOAT2 dstDim) 
    {
        XMFLOAT2 srcDim = {};
        D3D12_RESOURCE_DESC srcDesc = srcTex->GetDesc();
        srcDim.x = (float)srcDesc.Width;
        srcDim.y = (float)srcDesc.Height;

        if (dstDim.x == 0 || dstDim.y == 0)
        {
            D3D12_RESOURCE_DESC dstDesc = dstTex->GetDesc();
            dstDim.x = (float)dstDesc.Width;
            dstDim.y = (float)dstDesc.Height;
        }

        if (!cmdList || dstDim.x == 0.0f)
            return;

        Composition::Input inputs = {};
        inputs.Resources.InDenoisedRadiance = srcTex;

        const Composition::Constants constants = {
            .DstTexSize = { dstDim.x, dstDim.y, (1.0f / dstDim.x), (1.0f / dstDim.y) },
            .Flags = (UINT) CompFlags::RawSourceBlit | (UINT) CompFlags::ScaleSrc,
            .SrcTexSize = { std::min(srcDim.x, m_renderSize.x), std::min(srcDim.y, m_renderSize.y) }
        };

        std::array<ID3D12Resource*, 1> uavs { dstTex };
        const std::span<const byte> cbData((const byte*) &constants, sizeof(constants));

        m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, uavs, dstDim, false);
    }
};

// Public interface

FSRDPreprocessor_Dx12::FSRDPreprocessor_Dx12(std::string_view name, ID3D12Device* pDev)
    : m_impl(std::make_unique<Impl>()), m_InstanceName(name), m_IsInitialized(false)
{
    try
    {
        m_impl->m_pDev = pDev;
        m_impl->Initialize(GetAsByteSpan(FSRDFloorSeed_cso), GetAsByteSpan(FSRDFloor_cso),
                           GetAsByteSpan(FSRDInputConv_cso), GetAsByteSpan(FSRDOutputComp_cso));
        m_IsInitialized = true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD shaders failed to initialize. Details: {}", err.what());
    }
}

FSRDPreprocessor_Dx12::~FSRDPreprocessor_Dx12() = default;

bool FSRDPreprocessor_Dx12::IsInit() const { return m_IsInitialized; }

std::string_view FSRDPreprocessor_Dx12::GetName() const { return m_InstanceName; }

bool FSRDPreprocessor_Dx12::SetMaxRenderSize(UINT width, UINT height)
{ 
    try
    {
        m_impl->SetMaxRenderSize(width, height);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("Failed to resize FSRD buffers. Details: {}", err.what());
    }

    return false;
}

bool FSRDPreprocessor_Dx12::DispatchConversion(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
{ 
    try
    {
        m_impl->DispatchConversion(cmdList, desc);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD input conversion failed. Details: {}", err.what());
    }

    return false;
}

static void SetDescResources(Conversion::Output& descData, ffxDispatchDescHeader& signalHeader,
                             ffxDispatchDescDenoiser& dispatchDesc)
{
    auto& outResources = descData.Resources;

    dispatchDesc.header = 
    { 
        .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER,
        .pNext = &signalHeader // Link signal desc to main header
    };

    dispatchDesc.linearDepth = ffxApiGetResourceDX12(outResources.LinearDepth.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxApiGetResourceDX12(outResources.Motion.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchDesc.normals = ffxApiGetResourceDX12(outResources.Normals.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchDesc.specularAlbedo = ffxApiGetResourceDX12(outResources.SpecAlbedo.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchDesc.diffuseAlbedo = ffxApiGetResourceDX12(outResources.DiffAlbedo.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
}

void FSRDPreprocessor_Dx12::GetSignal(ffxDispatchDescDenoiserInput1Signal& signalDesc,
                                      ffxDispatchDescDenoiser& dispatchDesc) const
{
    auto& outResources = m_impl->m_out.Resources;

    signalDesc = { .header = { .type = FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER },
                   .radiance = { .input = ffxApiGetResourceDX12(outResources.Radiance.Get(),
                                                                FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
                                 .output = ffxApiGetResourceDX12(m_impl->m_outputBuffer1.Get(),
                                                                 FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) },
                   .fusedAlbedo = ffxApiGetResourceDX12(outResources.FusedAlbedo.Get(),
                                                        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ) };

    SetDescResources(m_impl->m_out, signalDesc.header, dispatchDesc);
}

void FSRDPreprocessor_Dx12::SetDenoiserOutputsWritable(ID3D12GraphicsCommandList* cmdList, bool writable)
{
    std::array<ID3D12Resource*, 1> buffers = { m_impl->m_outputBuffer1.Get() };
    AddBarriers(cmdList, buffers, writable ? kSrvState : kUavState, writable ? kUavState : kSrvState);
}

bool FSRDPreprocessor_Dx12::DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc)
{
    try
    {
        m_impl->DispatchComposition(cmdList, desc);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD output composition failed. Details: {}", err.what());
    }

    return false;
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetCompositionOutput() const 
{ 
    return m_impl->m_out.Resources.Motion.Get(); 
}

bool FSRDPreprocessor_Dx12::Blit(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* srcTex,
                                 ID3D12Resource* dstTex, XMFLOAT2 dim) const

{
    try
    {
        m_impl->Blit(cmdList, srcTex, dstTex, dim);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD blit failed. Details: {}", err.what());
    }

    return false;
}

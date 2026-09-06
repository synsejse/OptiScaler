#include "pch.h"
#include "FSRDPreprocessor_Dx12.h"
#include "FSRDShaderUtils.h"
#include "FSRDShaderData.h"
#include "resource_tracking/FSRDSubmission.h"
#include <fsrd_generated/FSRDInputConv_Shader.h>
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
}

struct ComputeState
{
    ID3D12Device* m_pDev = nullptr;
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    struct Storage
    {
        FrameDescriptorHeap heap;
        ComPtr<ID3D12Resource> constants;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
        std::vector<ComPtr<ID3D12Resource>> resources;
        byte* mapped = nullptr;

        ~Storage()
        {
            if (constants && mapped)
                constants->Unmap(0, nullptr);
        }
    };

    struct Slot
    {
        std::shared_ptr<Storage> storage;
        std::shared_ptr<FSRDSubmission::Ticket> ticket;
    };

    std::vector<Slot> m_slots;
    UINT m_cbSlotSize = 0;
    UINT m_numSrvs = 0;
    UINT m_numUavs = 0;
    std::wstring m_cbName;

    std::shared_ptr<Storage> AcquireStorage(ID3D12GraphicsCommandList* list)
    {
        auto slot = std::find_if(m_slots.begin(), m_slots.end(),
                                  [](const auto& item) { return FSRDSubmission::Complete(item.ticket); });
        if (slot == m_slots.end())
        {
            // The game can record more evaluations than its swapchain buffer count during reloads.
            // Grow without waiting on a command list that the caller has not submitted yet.
            if (m_slots.size() >= 64)
                throw std::runtime_error("FSRD dispatch storage limit reached; GPU work has not completed");

            auto storage = std::make_shared<Storage>();
            D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = m_cbSlotSize;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ThrowIfFailed(m_pDev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&storage->constants)),
                            "Failed to create FSRD dispatch constants");
            storage->constants->SetName(m_cbName.c_str());
            D3D12_RANGE noRead = { 0, 0 };
            ThrowIfFailed(storage->constants->Map(0, &noRead, reinterpret_cast<void**>(&storage->mapped)),
                            "Failed to map FSRD dispatch constants");
            if (!storage->heap.Initialize(m_pDev, m_numSrvs, m_numUavs, 0, 0))
                throw std::runtime_error("Failed to create FSRD dispatch descriptor heap");
            storage->root = m_rootSig;
            storage->pipeline = m_pso;
            m_slots.push_back({ std::move(storage), {} });
            slot = std::prev(m_slots.end());
            if (m_slots.size() == 4 || m_slots.size() == 8 || m_slots.size() == 16)
                LOG_INFO("FSRD dispatch storage expanded to {} slots for {}", m_slots.size(),
                           wstring_to_string(m_cbName));
        }

        // Register ownership BEFORE recording any GPU references. Storage survives destruction of
        // this feature and is never reused solely because some number of CPU frames has elapsed.
        slot->ticket = FSRDSubmission::Retain(m_pDev, list, slot->storage);
        return slot->storage;
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
        m_slots.reserve(backBufferCount);
        m_numSrvs = numSrvs;
        m_numUavs = numUavs;
        m_cbName = cbName;
        m_cbSlotSize = AlignTo256(cbDataSize);

        // Create Root Signature
        ThrowIfFailed(m_pDev->CreateRootSignature(0, bytecode.data(), bytecode.size(), IID_PPV_ARGS(&m_rootSig)),
              "Failed to create Root Signature");

        // Create PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS = { bytecode.data(), bytecode.size() };
        ThrowIfFailed(m_pDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)), "Failed to create PSO");

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

        if (cbData.size() > m_cbSlotSize || inputs.size() != m_numSrvs || output.size() != m_numUavs)
            throw std::runtime_error("FSRD dispatch layout does not match its shader");

        auto storage = AcquireStorage(cmdList);
        storage->resources.clear();
        for (auto* resource : inputs)
            storage->resources.emplace_back(resource);
        for (auto* resource : output)
            storage->resources.emplace_back(resource);
        memcpy(storage->mapped, cbData.data(), cbData.size());
        const auto cbAddress = storage->constants->GetGPUVirtualAddress();

        // Transitions SRV -> UAV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kSrvState, kUavState);

        // Update descriptors
        FrameDescriptorHeap& currentHeap = storage->heap;
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

    ComputeState m_convShader;
    ComputeState m_compShader;

    UINT m_maxWidth = 0;
    UINT m_maxHeight = 0;
    XMFLOAT2 m_renderSize = {};

    // Output Targets
    // Internal storage
    Conversion::Output m_out;
    ComPtr<ID3D12Resource> m_outputBuffer1;
    ComPtr<ID3D12Resource> m_compositionOutput;
    void Initialize(std::span<const byte> convByteCode, std::span<const byte> compByteCode)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        LOG_DEBUG("Creating FSRD interop shaders...");

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

        if (width == 0 || height == 0)
            throw std::runtime_error("FSRD render capacity must be nonzero");

        auto CreateTex = [&](DXGI_FORMAT fmt, LPCWSTR name, UINT mipLevels = 1)
        { 
            return CreateTexture2D(m_pDev, width, height, fmt, name, kSrvState, mipLevels);
        };

        // Publish a complete allocation only. A failed allocation must not leave a half-resized
        // converter whose dimensions falsely advertise readiness on a subsequent retry.
        Conversion::Output replacement;
        auto& outResources = replacement.Resources;
        outResources.Motion = CreateTex(FSRDFormats::Motion, L"FSR_Conv_Motion");
        outResources.Normals = CreateTex(FSRDFormats::Normals, L"FSR_Conv_Normals");
        outResources.SpecAlbedo = CreateTex(FSRDFormats::SpecAlbedo, L"FSR_Conv_SpecAlbedo");
        outResources.DiffAlbedo = CreateTex(FSRDFormats::DiffAlbedo, L"FSR_Conv_DiffAlbedo");
        outResources.LinearDepth = CreateTex(FSRDFormats::LinearDepth, L"FSR_Conv_LinearDepth");
        outResources.SkipSignal = CreateTex(FSRDFormats::SkipSignal, L"FSR_Conv_SkipSignal");

        auto outputBuffer = CreateTex(FSRDFormats::OutputBuffer1, L"FSR_Conv_OutputBuffer1");

        outResources.Radiance = CreateTex(FSRDFormats::Radiance, L"FSR_Conv_Radiance");
        outResources.FusedAlbedo = CreateTex(FSRDFormats::FusedAlbedo, L"FSR_Conv_FusedAlbedo");

        m_out = std::move(replacement);
        m_outputBuffer1 = std::move(outputBuffer);
        m_maxWidth = width;
        m_maxHeight = height;
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
            .PreviousDepthProjection = desc.PreviousDepthProjection,
            .RenderSize = desc.RenderSize,
            .NearPlane = desc.NearPlane,
            .FarPlane = desc.FarPlane,
            .Flags = desc.Flags
        };

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

        // DLSS-RR to FSR-RR conversion
        DispatchPackingShader(cmdList, desc);

    }

    void DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc, bool identityDenoiser)
    {
        if (!cmdList || !m_maxWidth)
            throw std::runtime_error("FSRD composition has no command list or allocated buffers");

        const UINT width = static_cast<UINT>(desc.DstTexSize.x);
        const UINT height = static_cast<UINT>(desc.DstTexSize.y);
        if (width == 0 || height == 0 || width > m_maxWidth || height > m_maxHeight)
            throw std::runtime_error("FSRD composition extent exceeds allocated buffers");

        // FSR's exposure pass may inspect the underlying allocation, not just renderSize. Never
        // give it inactive/uninitialized display-capacity padding as part of the composed color.
        // Recorded dispatch storage retains previous outputs until their submission completes.
        if (!m_compositionOutput || m_compositionOutput->GetDesc().Width != width ||
            m_compositionOutput->GetDesc().Height != height)
            m_compositionOutput = CreateTexture2D(m_pDev, width, height, FSRDFormats::Radiance,
                                                  L"FSR_Composed_Color", kSrvState);

        auto& outResources = m_out.Resources;
        Composition::Input inputs = {};
        Composition::Constants constants = 
        {
            .DstTexSize = desc.DstTexSize,
            .Flags = UINT(desc.Flags) 
        };

        inputs.Resources = { .InDenoisedRadiance = identityDenoiser ? outResources.Radiance.Get() : m_outputBuffer1.Get(),
                             .InFusedAlbedo = outResources.FusedAlbedo.Get(),
                             .InSkipSignal = outResources.SkipSignal.Get() };

        std::array<ID3D12Resource*, 1> uavs { m_compositionOutput.Get() };
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
        m_impl->Initialize(GetAsByteSpan(FSRDInputConv_cso), GetAsByteSpan(FSRDOutputComp_cso));
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

bool FSRDPreprocessor_Dx12::DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc,
                                             bool identityDenoiser)
{
    try
    {
        m_impl->DispatchComposition(cmdList, desc, identityDenoiser);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD output composition failed. Details: {}", err.what());
    }

    return false;
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetPreservedLighting() const
{
    return m_impl->m_out.Resources.SkipSignal.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetCompositionOutput() const 
{ 
    return m_impl->m_compositionOutput.Get();
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

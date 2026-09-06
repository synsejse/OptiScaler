#include "pch.h"
#include "Bias_Dx12.h"

#include "Bias_Common.h"
#include "precompile/Bias_Shader.h"

#include <Config.h>
#include <resource_tracking/FSRDSubmission.h>
#include <wrl/client.h>

struct Bias_Dx12::DispatchStorage
{
    FrameDescriptorHeap heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> constants;
    Microsoft::WRL::ComPtr<ID3D12Resource> input;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
};

std::shared_ptr<Bias_Dx12::DispatchStorage> Bias_Dx12::AcquireDispatchStorage(ID3D12GraphicsCommandList* commandList)
{
    auto slot = std::find_if(_dispatchSlots.begin(), _dispatchSlots.end(),
                             [](const auto& item) { return FSRDSubmission::Complete(item.ticket); });
    if (slot == _dispatchSlots.end())
    {
        // Recording count is not a GPU completion signal, including during loading/menu changes.
        if (_dispatchSlots.size() >= 64)
            throw std::runtime_error("Bias dispatch storage limit reached; GPU work has not completed");

        auto storage = std::make_shared<DispatchStorage>();
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InternalConstants));
        const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&storage->constants))))
            throw std::runtime_error("Failed to create Bias dispatch constants");
        storage->constants->SetName(L"Bias_Dispatch_Constants");
        if (!storage->heap.Initialize(_device, 1, 1, 1))
            throw std::runtime_error("Failed to create Bias dispatch descriptors");
        storage->root = _rootSignature;
        storage->pipeline = _pipelineState;
        _dispatchSlots.push_back({ std::move(storage), {} });
        slot = std::prev(_dispatchSlots.end());
    }

    // A pending submission keeps the descriptors, constants, and resources alive even if this
    // shader is destroyed or its output allocation changes before the GPU reaches this dispatch.
    slot->ticket = FSRDSubmission::Retain(_device, commandList, slot->storage);
    return slot->storage;
}

bool Bias_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState)
{
    if (!InDevice || !InSource)
        return false;

    if (_buffer)
    {
        const auto sourceDesc = InSource->GetDesc();
        const auto bufferDesc = _buffer->GetDesc();
        if (bufferDesc.Width == sourceDesc.Width && bufferDesc.Height == sourceDesc.Height &&
            bufferDesc.Format == sourceDesc.Format)
            return true; // Reusing storage must not reset its tracked state to the creation state.
    }

    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    auto result = Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &_buffer, resourceFlags);

    if (result)
    {
        _buffer->SetName(L"Bias_Buffer");
        _bufferState = InState;
    }

    return result;
}

void Bias_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState)
{
    return Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

bool Bias_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, float InBias,
                         ID3D12Resource* OutResource)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    LOG_DEBUG("[{0}] Start!", _name);

    try
    {
        auto storage = AcquireDispatchStorage(InCmdList);
        storage->input = InResource;
        storage->output = OutResource;
        FrameDescriptorHeap& currentHeap = storage->heap;

        CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
        CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

        InternalConstants constants {};
        constants.Bias = std::clamp(InBias, 0.0f, 0.9f);

        if (!CreateConstantsBuffer(_device, storage->constants.Get(), constants, currentHeap.GetCbvCPU(0)))
        {
            LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
            return false;
        }

        ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

        InCmdList->SetComputeRootSignature(_rootSignature);
        InCmdList->SetPipelineState(_pipelineState);

        InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

        const auto inDesc = InResource->GetDesc();
        const UINT dispatchWidth = static_cast<UINT>((inDesc.Width + InNumThreadsX - 1) / InNumThreadsX);
        const UINT dispatchHeight = (inDesc.Height + InNumThreadsY - 1) / InNumThreadsY;

        // Bias timing is not consumed by the overlay. Do not record an unrelated, unfenced
        // timestamp-ring allocation alongside this submission-owned shader work.
        InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);
        return true;
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("[{}] Dispatch failed: {}", _name, error.what());
        return false;
    }
}

Bias_Dx12::Bias_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 1, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, bias_cso, sizeof(bias_cso), biasShader.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = true; // Per-dispatch constants and descriptors are allocated lazily.
}

Bias_Dx12::~Bias_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_buffer);
}

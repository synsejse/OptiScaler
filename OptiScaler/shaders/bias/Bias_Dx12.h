#pragma once
#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>
#include <memory>
#include <vector>

namespace FSRDSubmission
{
struct Ticket;
}

class Bias_Dx12 : public Shader_Dx12
{
  private:
    struct alignas(256) InternalConstants
    {
        float Bias;
    };

    struct DispatchStorage;
    struct DispatchSlot
    {
        std::shared_ptr<DispatchStorage> storage;
        std::shared_ptr<FSRDSubmission::Ticket> ticket;
    };
    std::vector<DispatchSlot> _dispatchSlots;

    std::shared_ptr<DispatchStorage> AcquireDispatchStorage(ID3D12GraphicsCommandList* commandList);

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    UINT InNumThreadsX = 16;
    UINT InNumThreadsY = 16;

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, float InBias,
                  ID3D12Resource* OutResource);

    ID3D12Resource* Buffer() { return _buffer; }
    bool CanRender() const { return _init && _buffer != nullptr; }

    Bias_Dx12(std::string InName, ID3D12Device* InDevice);

    ~Bias_Dx12();
};

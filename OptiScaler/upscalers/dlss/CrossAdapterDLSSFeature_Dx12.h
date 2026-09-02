#pragma once

#include "DLSSFeature_Dx12.h"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class FT_Dx12;

enum class CrossAdapterNGXFeature
{
    SuperResolution,
    RayReconstruction,
};

// Common CPU-staged transport for NVIDIA NGX features running on a GPU other than the render adapter.
//
// vkd3d-proton does not implement the cross-adapter shared handles needed for a direct GPU-to-GPU path. Every
// input is therefore copied to render-device readback memory, copied by the CPU to NVIDIA upload memory, and
// recreated as an NVIDIA texture. Outputs take the reverse route. The main HDR output is packed from
// R16G16B16A16_FLOAT to R11G11B10_FLOAT for the trip back; every input and auxiliary output remains lossless.
// Capture and injection are recorded directly into the game's command list. NVIDIA evaluation consumes the
// preceding submitted capture, adding one frame of latency while preserving resource producer/consumer ordering.
class CrossAdapterNGXFeatureDx12 : public DLSSFeatureDx12
{
  private:
    inline static std::mutex _sharedRuntimeMutex;
    inline static Microsoft::WRL::ComPtr<ID3D12Device> _sharedNvidiaDevice;
    inline static bool _sharedNgxInitialized = false;

    struct TransferResource
    {
        const char* parameter = nullptr;
        const char* name = nullptr;
        bool required = false;
        bool output = false;
        bool packedOutput = false;

        Microsoft::WRL::ComPtr<ID3D12Resource> renderBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> nvidiaBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> nvidiaTexture;
        ID3D12Resource* frameResource = nullptr;
        std::byte* renderMappedData = nullptr;
        std::byte* nvidiaMappedData = nullptr;

        D3D12_RESOURCE_DESC sourceDesc {};
        D3D12_RESOURCE_DESC bridgeDesc {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT renderFootprint {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT nvidiaFootprint {};
        UINT renderRowCount = 0;
        UINT nvidiaRowCount = 0;
        UINT64 renderRowSize = 0;
        UINT64 nvidiaRowSize = 0;
        UINT64 renderTotalSize = 0;
        UINT64 nvidiaTotalSize = 0;
        D3D12_RESOURCE_STATES nvidiaTextureState = D3D12_RESOURCE_STATE_COPY_DEST;

        void Reset();
    };

    CrossAdapterNGXFeature _featureKind;
    const char* _featureLabel = nullptr;
    NVSDK_NGX_Feature _nativeFeature = NVSDK_NGX_Feature_SuperSampling;
    NVSDK_NGX_Handle _nativeHandle {};
    NVSDK_NGX_Handle* _pNativeHandle = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Device> _nvidiaDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _nvidiaQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _nvidiaAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _nvidiaCommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> _nvidiaFence;
    UINT64 _nvidiaFenceValue = 0;
    HANDLE _nvidiaFenceEvent = nullptr;

    static constexpr UINT NvidiaTimestampCount = 5;
    enum NvidiaTimestamp : UINT
    {
        UploadStart,
        UploadEnd,
        FeatureEnd,
        PackEnd,
        ReadbackEnd,
    };
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> _nvidiaTimestampHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> _nvidiaTimestampReadback;
    UINT64 _nvidiaTimestampFrequency = 0;
    bool _nvidiaGpuTimingEnabled = false;

    Microsoft::WRL::ComPtr<ID3D12Fence> _renderFence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _renderQueue;
    UINT64 _renderFenceValue = 0;
    HANDLE _renderFenceEvent = nullptr;

    NVSDK_NGX_Parameter* _nvidiaParameters = nullptr;
    std::mutex _pipelineMutex;
    std::mutex _statusMutex;
    CrossAdapterInfo _crossAdapterInfo;
    bool _capturePending = false;
    bool _primingWarningLogged = false;

    std::vector<std::unique_ptr<TransferResource>> _ownedTransfers;
    std::vector<TransferResource*> _transfers;
    TransferResource* _output = nullptr;
    uint64_t _transferredBytesPerFrame = 0;

    std::array<float, 16> _worldToViewMatrix {};
    std::array<float, 16> _viewToClipMatrix {};

    std::unique_ptr<FT_Dx12> _nvidiaOutputPacker;
    std::unique_ptr<FT_Dx12> _renderOutputUnpacker;
    Microsoft::WRL::ComPtr<ID3D12Resource> _nvidiaPackedOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> _renderPackedOutput;
    D3D12_RESOURCE_STATES _nvidiaPackedOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES _renderPackedOutputState = D3D12_RESOURCE_STATE_COPY_DEST;

    struct PipelineTiming
    {
        double pipelineMs = 0.0;
        bool hasNvidiaGpuTiming = false;
        double nvidiaTransferMs = 0.0;
        double nvidiaFeatureMs = 0.0;
    };

    void AddTransfer(const char* parameter, const char* name, bool required = false, bool output = false,
                     bool packedOutput = false);
    void ConfigureTransfers();
    const std::vector<TransferResource*>& Transfers() const { return _transfers; }

    bool CreateSecondaryDevice();
    bool CreateCommandObjects();
    bool CreateNvidiaGpuTimingResources();
    void RecordNvidiaTimestamp(NvidiaTimestamp timestamp);
    void ResolveNvidiaTimestamps();
    void ReadNvidiaGpuTiming(PipelineTiming& timing);
    bool CreatePackedOutputResources(const D3D12_RESOURCE_DESC& sourceDesc);
    void ResetPackedOutputResources();
    bool CreateTransferResource(TransferResource& transfer, ID3D12Resource* source);
    bool MapTransferResource(TransferResource& transfer);
    bool CollectFrameResources(NVSDK_NGX_Parameter* parameters);
    bool PrepareTransferResources();
    void UpdateTransferInfo();
    void SetPipelineState(CrossAdapterPipelineState state);
    void CompleteFrame(const PipelineTiming& timing);

    bool WaitForFence(ID3D12Fence* fence, UINT64 value, HANDLE eventHandle, const char* owner) const;
    bool ValidateRenderQueue(ID3D12CommandQueue* queue);
    bool WaitForRenderQueue(ID3D12CommandQueue* queue);
    bool ExecuteNvidiaCommands();

    bool CaptureInputs(ID3D12GraphicsCommandList* commandList);
    bool CopyInputsToNvidia();
    bool EvaluateNvidia(PipelineTiming& timing);
    bool ReadBackOutputs();
    bool InjectOutputs(ID3D12GraphicsCommandList* commandList);
    bool OutputDescriptionsMatch() const;
    static bool CopyCpuStagingBuffer(TransferResource& transfer, bool renderToNvidia);

    void CopyCreateParameters(NVSDK_NGX_Parameter* source, NVSDK_NGX_Parameter* destination) const;
    void CopyEvaluateParameters(NVSDK_NGX_Parameter* source, NVSDK_NGX_Parameter* destination);

    static bool GetResource(NVSDK_NGX_Parameter* parameters, const char* key, ID3D12Resource** resource);
    static bool SameResourceDescription(const D3D12_RESOURCE_DESC& left, const D3D12_RESOURCE_DESC& right);
    static D3D12_RESOURCE_STATES GetRenderResourceState(const TransferResource& transfer);
    static void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  protected:
    CrossAdapterNGXFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters,
                               CrossAdapterNGXFeature featureKind);

    virtual void ProcessNativeInitParams(NVSDK_NGX_Parameter* parameters);
    virtual void ProcessNativeEvaluateParams(NVSDK_NGX_Parameter* parameters);
    virtual void ReadNativeVersion();

    bool InitInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters) final;
    bool EvaluateInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters) final;

  public:
    ~CrossAdapterNGXFeatureDx12() override;
    std::optional<CrossAdapterInfo> GetCrossAdapterInfo() override;
    std::optional<double> ReadUpscalerTime(void* commandQueue) override;
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override;
};

class CrossAdapterDLSSFeatureDx12 final : public CrossAdapterNGXFeatureDx12
{
  public:
    CrossAdapterDLSSFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters);
};

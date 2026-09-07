#pragma once

#include "FSRDCyberpunkGuidePass.h"
#include <State.h>
#include <resource_tracking/FSRDSubmission.h>
#include <d3dcompiler.h>
#include <atomic>
#include <stdexcept>

namespace FSRD::CyberpunkExposurePass
{
// Integer loads preserve the seven original 32-bit words, including floating
// encodings, without arithmetic, normalization, or NaN/signed-zero conversion.
inline constexpr char Shader[] = R"(
struct Words { uint a; uint b; uint c; uint d; uint e; uint f; uint g; };
StructuredBuffer<Words> Input : register(t0);
RWTexture2D<uint4> Output : register(u0);
[numthreads(1, 1, 1)]
void CSMain()
{
    Words words = Input[0];
    Output[uint2(0, 0)] = uint4(words.a, words.b, words.c, words.d);
    Output[uint2(1, 0)] = uint4(words.e, words.f, words.g, 0u);
}
)";

// Optional one-shot diagnostic, not an exposure policy. Caller must prove that
// source is the CURRENT original lighting VS+PS t37 structured SRV: stride28,
// first element0, at least one element, correct buffer registry/native identity,
// and no unsupported dynamic/suballocated path. This helper cannot inspect a
// CPU descriptor's contents. It copies the actual admitted descriptor unchanged.
// EngineAccess must admit/request the source's combined shader-read state before
// Record and restore original bindings/PSO afterward, including failed recording.
class Work : public std::enable_shared_from_this<Work>
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D12Device> device_;
    CyberpunkGuidePass::SourceView source_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> output_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_;
    UINT stride_ = 0;
    std::atomic<bool> attempted_ = false;

    static void Require(bool ok) { if (!ok) throw std::runtime_error("private exposure-word pass refused"); }
    static ComPtr<IUnknown> Identity(IUnknown* object)
    {
        ComPtr<IUnknown> result;
        Require(object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result))));
        return result;
    }
    static bool SameDevice(ID3D12DeviceChild* object, ID3D12Device* expected)
    {
        ComPtr<ID3D12Device> device;
        return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&device))) &&
            Identity(device.Get()).Get() == Identity(expected).Get();
    }
    Work() = default;

public:
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    static std::shared_ptr<Work> Prepare(ID3D12Device* device,
                                         const CyberpunkGuidePass::SourceView& source) noexcept
    {
        try
        {
            Require(device && source.resource && source.heap && source.descriptor.ptr);
            Require(SameDevice(source.resource.Get(), device) && SameDevice(source.heap.Get(), device));
            const auto input = source.resource->GetDesc();
            Require(input.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && input.Width >= 28 &&
                input.Width <= 1024 * 1024 && input.Height == 1 && input.DepthOrArraySize == 1 &&
                input.MipLevels == 1 && input.Format == DXGI_FORMAT_UNKNOWN && input.SampleDesc.Count == 1 &&
                input.SampleDesc.Quality == 0 && !(input.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
            const auto sourceHeap = source.heap->GetDesc();
            Require(sourceHeap.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
                    sourceHeap.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
            const auto stride = device->GetDescriptorHandleIncrementSize(sourceHeap.Type);
            const auto first = source.heap->GetCPUDescriptorHandleForHeapStart().ptr;
            Require(stride && source.descriptor.ptr >= first && (source.descriptor.ptr - first) % stride == 0 &&
                    (source.descriptor.ptr - first) / stride < sourceHeap.NumDescriptors);
            auto work = std::shared_ptr<Work>(new Work);
            work->device_ = device; work->source_ = source; work->stride_ = stride;
            ScopedSkipHeapCapture skipHeapCapture {};

            D3D12_DESCRIPTOR_RANGE ranges[2] = {
                { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 }
            };
            D3D12_ROOT_PARAMETER parameters[2] {};
            for (UINT i = 0; i < 2; ++i)
            {
                parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[i].DescriptorTable = { 1, &ranges[i] };
            }
            D3D12_ROOT_SIGNATURE_DESC rootDesc {};
            rootDesc.NumParameters = 2; rootDesc.pParameters = parameters;
            ComPtr<ID3DBlob> serialized, errors, shader;
            Require(SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                         &serialized, &errors)));
            Require(SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                serialized->GetBufferSize(), IID_PPV_ARGS(&work->root_))));
            Require(SUCCEEDED(D3DCompile(Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr,
                "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
            pipeline.pRootSignature = work->root_.Get();
            pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            Require(SUCCEEDED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&work->pso_))));
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 2; heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Require(SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&work->heap_))));
            auto descriptor = work->heap_->GetCPUDescriptorHandleForHeapStart();
            device->CopyDescriptorsSimple(1, descriptor, source.descriptor, heapDesc.Type);

            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            properties.CreationNodeMask = 1; properties.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC output {};
            output.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            output.Width = 2; output.Height = 1; output.DepthOrArraySize = 1;
            output.MipLevels = 1; output.SampleDesc.Count = 1;
            output.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            output.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const auto allocation = device->GetResourceAllocationInfo(0, 1, &output).SizeInBytes;
            Require(allocation && allocation <= 1024 * 1024);
            Require(SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &output,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&work->output_))));
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = output.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            descriptor.ptr += stride;
            device->CreateUnorderedAccessView(work->output_.Get(), nullptr, &uav, descriptor);
            return work;
        }
        catch (...) { return {}; }
    }

    // Only private compute/one private-output barrier. No engine memory writes,
    // scene copies, input barriers, OM/RS/IA changes or queue waits/submissions.
    bool Record(ID3D12GraphicsCommandList* list) noexcept
    {
        if (attempted_.exchange(true)) return false;
        try
        {
            Require(list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && SameDevice(list, device_.Get()));
            Require(bool(FSRDSubmission::Retain(device_.Get(), list, shared_from_this())));
            list->SetPipelineState(pso_.Get());
            list->SetComputeRootSignature(root_.Get());
            ID3D12DescriptorHeap* heaps[] = { heap_.Get() };
            list->SetDescriptorHeaps(1, heaps);
            auto descriptor = heap_->GetGPUDescriptorHandleForHeapStart();
            list->SetComputeRootDescriptorTable(0, descriptor);
            descriptor.ptr += stride_;
            list->SetComputeRootDescriptorTable(1, descriptor);
            list->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = output_.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);
            return true;
        }
        catch (...) { return false; }
    }

    const ComPtr<ID3D12Resource>& Output() const { return output_; }
};
} // namespace FSRD::CyberpunkExposurePass

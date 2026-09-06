#include "pch.h"
#include "FSRDResearchCapture.h"
#include <DirectXMath.h>
#include "shaders/fsrd_preprocess/FSRDShaderUtils.h"
#include "resource_tracking/ResTrack_dx12.h"
#include "Util.h"
#include <fsrd_generated/FSRDResearchCopy_Shader.h>
#include <json.hpp>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <include/d3dx/d3dx12.h>

namespace FSRDResearch
{
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
constexpr UINT MaxTextures = 48;
constexpr UINT64 MaxBytes = 512ull * 1024 * 1024;
static std::atomic<bool> hasCaptures { false };

struct Entry
{
    std::string name;
    UINT width, height;
    UINT64 bytes;
    ComPtr<ID3D12Resource> readback;
};

struct Batch
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12GraphicsCommandList> list;
    ID3D12CommandList* submittedList = nullptr;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> scratch;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> sources;
    std::vector<Entry> entries;
    Json metadata;
    std::filesystem::path directory;
    UINT width, height, increment;
    UINT64 bytes = 0;
    bool submitted = false;
    bool signalFailed = false;
};

struct Registry
{
    std::mutex mutex;
    std::vector<Capture> pending;
    UINT count = 0;
    UINT burst = 0;
    std::string label;
    uint64_t burstFeature = 0;
};

static Registry& RegistryInstance()
{
    // Unsubmitted/failed batches must not free resources still referenced by a command list.
    // The bounded registry deliberately survives DLL/process shutdown (no unsafe GPU wait).
    static auto* registry = new Registry;
    return *registry;
}

static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type)
{
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    if (type == D3D12_HEAP_TYPE_DEFAULT)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    ComPtr<ID3D12Resource> result;
    FSRD::ThrowIfFailed(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                                        type == D3D12_HEAP_TYPE_READBACK
                                                            ? D3D12_RESOURCE_STATE_COPY_DEST
                                                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr, IID_PPV_ARGS(&result)),
                        "research buffer allocation failed");
    return result;
}

Capture Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT width, UINT height, uint64_t feature,
              uint64_t frame, const std::string& metadata)
{
    if (!Config::Instance()->FfxDenoiserResearchCapture.value_or_default())
        return {};
    auto& registry = RegistryInstance();
    std::lock_guard lock(registry.mutex);
    if (registry.pending.size() >= 2 || registry.count >= 12 || width == 0 || height == 0 ||
        UINT64(width) * height * 16 > MaxBytes / 2)
        return {};
    try
    {
        const auto base = Util::ExePath().parent_path();
        const auto request = base / "FSRRR-capture.request";
        std::error_code error;
        if (registry.burst == 0)
        {
            if (!std::filesystem::exists(request, error))
                return {};
            std::ifstream file(request);
            std::getline(file, registry.label);
            if (registry.label.empty() || registry.label.size() > 48 ||
                registry.label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                    std::string::npos)
                return {};
            file.close();
            // Consume only our explicit diagnostic trigger; no arbitrary paths from its contents.
            if (!std::filesystem::remove(request, error))
                return {};
            registry.burst = 2;
            registry.burstFeature = feature;
        }
        if (registry.burstFeature != feature)
            return {};

        auto batch = std::make_shared<Batch>();
        batch->device = device;
        batch->list = list;
        batch->submittedList = ResTrack_Dx12::PrepareResearchSubmission(device, list);
        if (!batch->submittedList)
            throw std::runtime_error("research submission hook unavailable");
        batch->width = width;
        batch->height = height;
        batch->metadata = Json::parse(metadata);
        batch->metadata["feature"] = feature;
        batch->metadata["frame"] = frame;
        batch->metadata["storage"] =
            "little-endian float32 RGBA, row-major, shader-visible values (not native texture bytes)";
        batch->metadata["textures"] = Json::array();
        batch->directory =
            base / "FSRRR-captures" / std::format("{}-{}-{}-{}", registry.label, GetCurrentProcessId(), feature, frame);
        std::filesystem::create_directories(batch->directory);
        ScopedSkipHeapCapture skip;
        FSRD::ThrowIfFailed(device->CreateRootSignature(0, FSRDResearchCopy_cso, sizeof(FSRDResearchCopy_cso),
                                                        IID_PPV_ARGS(&batch->root)),
                            "research root signature failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = batch->root.Get();
        pipeline.CS = { FSRDResearchCopy_cso, sizeof(FSRDResearchCopy_cso) };
        FSRD::ThrowIfFailed(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&batch->pso)),
                            "research PSO failed");
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = MaxTextures;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        FSRD::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&batch->heap)),
                            "research descriptors failed");
        batch->increment = device->GetDescriptorHandleIncrementSize(heap.Type);
        batch->scratch = Buffer(device, UINT64(width) * height * 16, D3D12_HEAP_TYPE_DEFAULT);
        FSRD::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&batch->fence)),
                            "research fence failed");
        registry.pending.push_back(batch);
        hasCaptures.store(true, std::memory_order_release);
        ++registry.count;
        --registry.burst;
        LOG_INFO("FSRRR research recording {}", batch->directory.string());
        return batch;
    }
    catch (const std::exception& error)
    {
        registry.burst = 0;
        LOG_ERROR("FSRRR research begin: {}", error.what());
        return {};
    }
}

void Record(const Capture& batch, const char* name, ID3D12Resource* texture)
{
    if (!batch)
        return;
    Json meta = { { "name", name }, { "present", texture != nullptr } };
    if (!texture)
    {
        batch->metadata["textures"].push_back(meta);
        return;
    }
    const auto desc = texture->GetDesc();
    const auto format = FSRD::GetViewFormat(desc.Format);
    meta["resource_format"] = UINT(desc.Format);
    meta["view_format"] = UINT(format);
    meta["resource_width"] = desc.Width;
    meta["resource_height"] = desc.Height;
    meta["resource_flags"] = UINT(desc.Flags);
    meta["resource_address"] = std::format("{:x}", reinterpret_cast<uintptr_t>(texture));
    // Never interpret integer, compressed, multisampled or unknown-format textures as floats.
    const bool supported = format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32_FLOAT ||
                           format == DXGI_FORMAT_R32G32B32A32_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT ||
                           format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R16_FLOAT ||
                           format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R8_UNORM ||
                           format == DXGI_FORMAT_R16_UNORM;
    const UINT width = UINT(std::min<UINT64>(desc.Width, batch->width));
    const UINT height = std::min(desc.Height, batch->height);
    const UINT64 bytes = UINT64(width) * height * 16;
    if (!supported || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        batch->entries.size() >= MaxTextures || batch->bytes + bytes > MaxBytes)
    {
        meta["status"] = "metadata only: unsupported layout or capture budget";
        batch->metadata["textures"].push_back(meta);
        return;
    }
    try
    {
        ScopedSkipHeapCapture skip;
        Entry entry { name, width, height, bytes, Buffer(batch->device.Get(), bytes, D3D12_HEAP_TYPE_READBACK) };
        const UINT slot = UINT(batch->entries.size());
        // Register owning objects BEFORE recording anything that references them.
        batch->entries.push_back(entry);
        batch->sources.emplace_back(texture);
        batch->bytes += bytes;
        auto cpu = batch->heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * batch->increment;
        auto gpu = batch->heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * batch->increment;
        FSRD::CreateSRV(batch->device.Get(), cpu, texture);
        auto* list = batch->list.Get();
        ID3D12DescriptorHeap* heaps[] = { batch->heap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(batch->root.Get());
        list->SetPipelineState(batch->pso.Get());
        const UINT dimensions[] = { width, height };
        list->SetComputeRoot32BitConstants(0, 2, dimensions, 0);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRootUnorderedAccessView(2, batch->scratch->GetGPUVirtualAddress());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        FSRD::AddBarrier(list, batch->scratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(entry.readback.Get(), 0, batch->scratch.Get(), 0, bytes);
        FSRD::AddBarrier(list, batch->scratch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        meta["width"] = width;
        meta["height"] = height;
        meta["file"] = entry.name + ".f32";
    }
    catch (const std::exception& error)
    {
        meta["error"] = error.what();
    }
    batch->metadata["textures"].push_back(meta);
}

void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!hasCaptures.load(std::memory_order_acquire))
        return;
    auto& registry = RegistryInstance();
    std::lock_guard lock(registry.mutex);
    for (auto& batch : registry.pending)
    {
        if (batch->submitted || batch->signalFailed)
            continue;
        if (std::find(lists, lists + count, batch->submittedList) == lists + count)
            continue;
        // This signal is on the actual submitting queue, after the containing command list.
        if (SUCCEEDED(queue->Signal(batch->fence.Get(), 1)))
        {
            batch->submitted = true;
            LOG_INFO("FSRRR research submitted {}", batch->directory.string());
        }
        else
            batch->signalFailed = true; // Keep bounded resources alive, never map incomplete GPU work.
    }
}

void Poll()
{
    auto& registry = RegistryInstance();
    std::lock_guard lock(registry.mutex);
    for (auto it = registry.pending.begin(); it != registry.pending.end();)
    {
        const auto& batch = *it;
        const UINT64 completed = batch->fence->GetCompletedValue();
        if (!batch->submitted || completed == UINT64_MAX || completed < 1)
        {
            ++it;
            continue;
        }
        try
        {
            for (const auto& entry : batch->entries)
            {
                void* data = nullptr;
                D3D12_RANGE read { 0, SIZE_T(entry.bytes) };
                FSRD::ThrowIfFailed(entry.readback->Map(0, &read, &data), "research readback map failed");
                std::ofstream file(batch->directory / (entry.name + ".f32"), std::ios::binary);
                file.write(static_cast<const char*>(data), std::streamsize(entry.bytes));
                const bool written = bool(file);
                D3D12_RANGE noWrites { 0, 0 };
                entry.readback->Unmap(0, &noWrites);
                if (!written)
                    throw std::runtime_error("research texture write failed");
            }
            // Manifest appears only after all texture writes and verified GPU completion.
            std::ofstream manifest(batch->directory / "manifest.json");
            manifest << batch->metadata.dump(2);
            LOG_INFO("FSRRR research saved {} textures to {}", batch->entries.size(), batch->directory.string());
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("FSRRR research save: {}", error.what());
        }
        it = registry.pending.erase(it);
    }
    hasCaptures.store(!registry.pending.empty(), std::memory_order_release);
}
} // namespace FSRDResearch

#include "pch.h"
#include "FSRDCyberpunkFogProbe.h"
#include "FSRDFogLayerCapture.h"

#include <Util.h>
#include <resource_tracking/FSRDSubmission.h>
#include <hooks/Hook_Utils.h>
#include <detours/detours.h>
#include <include/d3dx/d3dx12.h>
#include <json.hpp>
#include <bcrypt.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FSRDCyberpunkFogProbe
{
namespace
{
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;
constexpr uintptr_t FogNodeRva = 0x61f9e0;
constexpr std::string_view ExeSha256 = "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991";
constexpr std::array<unsigned char, 30> FogPrologue = {
    0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d,
    0x6c, 0x24, 0xa0, 0x48, 0x81, 0xec, 0x60, 0x01, 0x00, 0x00,
};
struct ShaderIdentity
{
    const char* name;
    SIZE_T bytes;
    std::string_view sha256;
};
constexpr ShaderIdentity FogShaders[] = {
    { "High", 8075, "a7a57220b8f5c1abddd8205d626ece403df647152d9a7afe147d5c285bc6589a" },
    { "Mid", 7251, "79fc7accdf41dd02a41d101effc20786fb809c867bc31502abc7c51548130f78" },
    { "Low", 6127, "3d2bfd8ac57ac8673accb3fa48757e1848481b0a3ef5ade3feba6153095ccf6b" },
    { "Simple", 2631, "0953f807c7a784c8012fe37eb46bd3ea40ecb17aa94baba36e10ad61d1206738" },
};
constexpr unsigned MaxDrawLogs = 32;
constexpr unsigned MaxPsoLogs = 16;
constexpr unsigned MaxRearms = 2;
constexpr SIZE_T FogVertexBytes = 2361;
constexpr std::string_view FogVertexSha256 = "174ce05e0a97ce65f80358b2a01bbadea4c314fb386cfac6064940a870f91a5a";
constexpr size_t MaxRtvHeaps = 64, MaxRtvSlots = 65536, MaxCommandLists = 128;
constexpr UINT64 MaxCaptureTextureBytes = 256ull * 1024 * 1024;
constexpr unsigned MaxNgxEndpoints = 8;
constexpr UINT64 MaxEndpointResourceBytes = 256ull * 1024 * 1024;

// Runtime loading avoids adding another DLL import to ordinary, probe-off builds.
struct CryptoApi
{
    HMODULE module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&BCryptOpenAlgorithmProvider) open = nullptr;
    decltype(&BCryptCreateHash) create = nullptr;
    decltype(&BCryptHashData) update = nullptr;
    decltype(&BCryptFinishHash) finish = nullptr;
    decltype(&BCryptDestroyHash) destroy = nullptr;
    BCRYPT_ALG_HANDLE algorithm = nullptr;

    CryptoApi()
    {
        if (!module)
            return;
        open = reinterpret_cast<decltype(open)>(GetProcAddress(module, "BCryptOpenAlgorithmProvider"));
        create = reinterpret_cast<decltype(create)>(GetProcAddress(module, "BCryptCreateHash"));
        update = reinterpret_cast<decltype(update)>(GetProcAddress(module, "BCryptHashData"));
        finish = reinterpret_cast<decltype(finish)>(GetProcAddress(module, "BCryptFinishHash"));
        destroy = reinterpret_cast<decltype(destroy)>(GetProcAddress(module, "BCryptDestroyHash"));
        if (!open || !create || !update || !finish || !destroy ||
            open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            algorithm = nullptr;
    }
};

struct Hash
{
    CryptoApi& api;
    BCRYPT_HASH_HANDLE handle = nullptr;
    bool valid = false;
    explicit Hash(CryptoApi& crypto) : api(crypto)
    {
        valid = api.algorithm && api.create(api.algorithm, &handle, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Hash() { if (handle) api.destroy(handle); }
    void Add(const void* bytes, ULONG count)
    {
        valid = valid && api.update(handle, const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)), count, 0) >= 0;
    }
    std::string Finish()
    {
        std::array<UCHAR, 32> result {};
        if (!valid || api.finish(handle, result.data(), ULONG(result.size()), 0) < 0)
            return {};
        std::string hex;
        for (auto value : result)
            hex += std::format("{:02x}", value);
        return hex;
    }
};

struct TaggedPso
{
    ComPtr<ID3D12PipelineState> pso;
    const char* shader;
    ComPtr<ID3D12PipelineState> authored;
    ComPtr<ID3D12RootSignature> root;
    std::vector<BYTE> vertexBytes, pixelBytes;
    D3D12_RENDER_TARGET_BLEND_DESC blend {};
    std::string pixelSha256;
};
struct RtvSlot
{
    // The descriptor itself does not own the resource. This pointer is borrowed
    // ONLY at its original valid OM bind, then AddRef'd before any GPU/async work.
    ID3D12Resource* resource = nullptr;
    D3D12_RENDER_TARGET_VIEW_DESC view {};
    uint64_t generation = 0;
    bool known = false;
};
struct RtvHeap
{
    ComPtr<ID3D12DescriptorHeap> heap; // Keeps the CPU handle range from recycling.
    SIZE_T start = 0;
    UINT increment = 0;
    uint64_t generation = 0;
    std::vector<RtvSlot> slots;
};
struct BoundRtv
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_RENDER_TARGET_VIEW_DESC view {};
    uint64_t heapGeneration = 0, slotGeneration = 0;
    UINT index = 0;
    bool known = false;
};
struct Query
{
    ID3D12QueryHeap* heap;
    D3D12_QUERY_TYPE type;
    UINT index;
};
struct ListState
{
    uint64_t generation = 0;
    // Ordinal of observed endpoints only, not of every command or GPU event.
    uint64_t endpointOrdinal = 0;
    bool known = false, predicated = false, renderPass = false;
    std::array<Query, 64> queries {};
    UINT queryCount = 0, viewportCount = 0, scissorCount = 0;
    std::array<D3D12_VIEWPORT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports {};
    std::array<D3D12_RECT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors {};
};
struct EndpointTrace
{
    ComPtr<IUnknown> list;
    // The first owned identity is the captured fog target. Keeping identities
    // alive through this short window prevents address recycling between logs.
    std::vector<ComPtr<IUnknown>> resources;
    Json fog;
    uint64_t generation = 0, ordinal = 0;
    UINT64 resourceBytes = 0;
    unsigned count = 0;
};
struct Registry
{
    std::mutex mutex;
    CryptoApi crypto;
    std::vector<TaggedPso> tagged;
    std::vector<RtvHeap> heaps;
    std::unordered_map<IUnknown*, ListState> lists;
    std::shared_ptr<EndpointTrace> endpoint;
    size_t slots = 0;
    uint64_t nextHeap = 0, nextSlot = 0, nextRecording = 0;
};
Registry& Data()
{
    // Bounded research objects survive DLL/process teardown. A held PSO cannot be
    // recycled to a different shader while its pointer appears in our provenance.
    static auto* data = new Registry;
    return *data;
}
std::once_flag initialization;
std::atomic<bool> active { false };
std::atomic<bool> captureEnabled { false }, captureTrackingValid { true };
std::atomic<unsigned> captureRefusalLogs { 0 };
std::atomic<bool> captureStarted { false };
std::atomic<bool> endpointActive { false };
std::atomic<unsigned> drawLogs { 0 }, emptyLogs { 0 };
std::atomic<unsigned> arm { 0 };
std::atomic<uint64_t> scopes { 0 };
std::mutex hookMutex;

using FogNode = void(__fastcall*)(void* node, void* context);
FogNode originalFogNode = nullptr;
using SetPso = rewrite_signature<decltype(&ID3D12GraphicsCommandList::SetPipelineState)>::type;
using SetRtv = rewrite_signature<decltype(&ID3D12GraphicsCommandList::OMSetRenderTargets)>::type;
using Draw = rewrite_signature<decltype(&ID3D12GraphicsCommandList::DrawInstanced)>::type;
using DrawIndexed = rewrite_signature<decltype(&ID3D12GraphicsCommandList::DrawIndexedInstanced)>::type;
using CreateGraphics = rewrite_signature<decltype(&ID3D12Device::CreateGraphicsPipelineState)>::type;
using CreateStream = rewrite_signature<decltype(&ID3D12Device2::CreatePipelineState)>::type;
SetPso originalSetPso = nullptr;
SetRtv originalSetRtv = nullptr;
Draw originalDraw = nullptr;
DrawIndexed originalDrawIndexed = nullptr;
CreateGraphics originalCreateGraphics = nullptr;
CreateStream originalCreateStream = nullptr;
using CreateHeap = rewrite_signature<decltype(&ID3D12Device::CreateDescriptorHeap)>::type;
using CreateRtv = rewrite_signature<decltype(&ID3D12Device::CreateRenderTargetView)>::type;
using CreateList = rewrite_signature<decltype(&ID3D12Device::CreateCommandList)>::type;
using CopyDescriptors = rewrite_signature<decltype(&ID3D12Device::CopyDescriptors)>::type;
using CopyDescriptorsSimple = rewrite_signature<decltype(&ID3D12Device::CopyDescriptorsSimple)>::type;
using Reset = rewrite_signature<decltype(&ID3D12GraphicsCommandList::Reset)>::type;
using ClearState = rewrite_signature<decltype(&ID3D12GraphicsCommandList::ClearState)>::type;
using SetPredication = rewrite_signature<decltype(&ID3D12GraphicsCommandList::SetPredication)>::type;
using BeginQuery = rewrite_signature<decltype(&ID3D12GraphicsCommandList::BeginQuery)>::type;
using EndQuery = rewrite_signature<decltype(&ID3D12GraphicsCommandList::EndQuery)>::type;
using ExecuteBundle = rewrite_signature<decltype(&ID3D12GraphicsCommandList::ExecuteBundle)>::type;
using SetViewports = rewrite_signature<decltype(&ID3D12GraphicsCommandList::RSSetViewports)>::type;
using SetScissors = rewrite_signature<decltype(&ID3D12GraphicsCommandList::RSSetScissorRects)>::type;
using BeginRenderPass = rewrite_signature<decltype(&ID3D12GraphicsCommandList4::BeginRenderPass)>::type;
using EndRenderPass = rewrite_signature<decltype(&ID3D12GraphicsCommandList4::EndRenderPass)>::type;
CreateHeap originalCreateHeap = nullptr;
CreateRtv originalCreateRtv = nullptr;
CreateList originalCreateList = nullptr;
CopyDescriptors originalCopyDescriptors = nullptr;
CopyDescriptorsSimple originalCopyDescriptorsSimple = nullptr;
Reset originalReset = nullptr;
ClearState originalClearState = nullptr;
SetPredication originalSetPredication = nullptr;
BeginQuery originalBeginQuery = nullptr;
EndQuery originalEndQuery = nullptr;
ExecuteBundle originalExecuteBundle = nullptr;
SetViewports originalSetViewports = nullptr;
SetScissors originalSetScissors = nullptr;
BeginRenderPass originalBeginRenderPass = nullptr;
EndRenderPass originalEndRenderPass = nullptr;

struct Scope
{
    Scope* previous;
    uint64_t serial;
    void* node;
    void* context;
    ID3D12GraphicsCommandList* psoList = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12GraphicsCommandList* rtvList = nullptr;
    UINT rtvCount = 0;
    BOOL contiguous = FALSE;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
    bool hasDsv = false;
    unsigned draws = 0;
    BoundRtv boundRtv;
};
thread_local Scope* scope = nullptr;
thread_local bool inMetadata = false;

// Diagnostics must never throw through a game/D3D entry point or suppress its call.
template <typename Fn> void Metadata(Fn&& fn) noexcept
{
    if (inMetadata)
        return;
    inMetadata = true;
    try { fn(); } catch (...) {}
    inMetadata = false;
}

template <typename Fn> void Track(Fn&& fn) noexcept
{
    if (!captureEnabled.load() || !captureTrackingValid.load() || inMetadata)
        return;
    inMetadata = true;
    try { fn(); }
    catch (const std::exception& error)
    {
        captureTrackingValid.store(false);
        try { LOG_WARN("[FSRRR fog capture] provenance tracking disabled: {}", error.what()); } catch (...) {}
    }
    catch (...) { captureTrackingValid.store(false); }
    inMetadata = false;
}

RtvSlot* FindRtv(Registry& data, SIZE_T handle, RtvHeap** owner = nullptr, UINT* index = nullptr)
{
    for (auto& heap : data.heaps)
    {
        if (handle < heap.start || !heap.increment)
            continue;
        const auto delta = handle - heap.start;
        const auto slot = delta / heap.increment;
        if (delta % heap.increment || slot >= heap.slots.size())
            continue;
        if (owner) *owner = &heap;
        if (index) *index = UINT(slot);
        return &heap.slots[slot];
    }
    return nullptr;
}

HRESULT WINAPI HookCreateHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                               REFIID iid, void** result)
{
    const HRESULT hr = originalCreateHeap(device, desc, iid, result);
    if (SUCCEEDED(hr) && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV && result && *result)
        Track([&] {
            ComPtr<ID3D12DescriptorHeap> heap;
            if (FAILED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&heap))))
                throw std::runtime_error("RTV heap identity unavailable");
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (const auto& known : data.heaps)
                if (known.heap.Get() == heap.Get())
                    return;
            if (!desc->NumDescriptors || data.heaps.size() >= MaxRtvHeaps ||
                desc->NumDescriptors > MaxRtvSlots - data.slots)
                throw std::runtime_error("RTV provenance budget exhausted");
            RtvHeap record;
            record.heap = heap;
            record.start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
            record.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            record.generation = ++data.nextHeap;
            record.slots.resize(desc->NumDescriptors);
            data.slots += desc->NumDescriptors;
            data.heaps.push_back(std::move(record));
        });
    return hr;
}

void WINAPI HookCreateRtv(ID3D12Device* device, ID3D12Resource* resource,
                          const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    originalCreateRtv(device, resource, desc, destination);
    Track([&] {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        if (auto* slot = FindRtv(data, destination.ptr))
        {
            *slot = {};
            slot->generation = ++data.nextSlot;
            // Null/default descriptors are deliberately not reconstructed by guesswork.
            if (resource && desc)
            {
                slot->resource = resource;
                slot->view = *desc;
                slot->known = true;
            }
        }
    });
}

void TrackDescriptorCopy(ID3D12Device* device, UINT destinationCount,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* destinations, const UINT* destinationSizes,
                          UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes)
{
    if (destinationCount > MaxRtvSlots || sourceCount > MaxRtvSlots || !destinations || !sources)
        throw std::runtime_error("RTV descriptor-copy layout unsupported");
    auto& data = Data();
    std::lock_guard lock(data.mutex);
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    std::vector<RtvSlot> snapshot;
    for (UINT range = 0; range < sourceCount; ++range)
    {
        const UINT count = sourceSizes ? sourceSizes[range] : 1;
        if (count > MaxRtvSlots - snapshot.size())
            throw std::runtime_error("RTV descriptor-copy budget exhausted");
        for (UINT i = 0; i < count; ++i)
        {
            const auto* slot = FindRtv(data, sources[range].ptr + SIZE_T(i) * increment);
            snapshot.push_back(slot ? *slot : RtvSlot {});
        }
    }
    size_t cursor = 0;
    for (UINT range = 0; range < destinationCount; ++range)
    {
        const UINT count = destinationSizes ? destinationSizes[range] : 1;
        if (count > snapshot.size() - cursor)
            throw std::runtime_error("RTV descriptor-copy counts disagree");
        for (UINT i = 0; i < count; ++i, ++cursor)
            if (auto* slot = FindRtv(data, destinations[range].ptr + SIZE_T(i) * increment))
            {
                *slot = snapshot[cursor]; // Unknown sources invalidate destinations too.
                slot->generation = ++data.nextSlot;
            }
    }
    if (cursor != snapshot.size())
        throw std::runtime_error("RTV descriptor-copy counts disagree");
}

void WINAPI HookCopyDescriptors(ID3D12Device* device, UINT destinationCount,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* destinations, const UINT* destinationSizes,
                                 UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources,
                                 const UINT* sourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        Track([&] { TrackDescriptorCopy(device, destinationCount, destinations, destinationSizes,
                                         sourceCount, sources, sourceSizes); });
    originalCopyDescriptors(device, destinationCount, destinations, destinationSizes, sourceCount, sources, sourceSizes, type);
}
void WINAPI HookCopyDescriptorsSimple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                       D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        Track([&] { TrackDescriptorCopy(device, 1, &destination, &count, 1, &source, &count); });
    originalCopyDescriptorsSimple(device, count, destination, source, type);
}

ComPtr<IUnknown> ListIdentity(ID3D12GraphicsCommandList* list)
{
    ComPtr<IUnknown> identity;
    if (!list || FAILED(list->QueryInterface(IID_PPV_ARGS(&identity))))
        throw std::runtime_error("command-list COM identity unavailable");
    return identity;
}

ComPtr<IUnknown> EndpointListIdentity(ID3D12GraphicsCommandList* list)
{
    if (!list)
        throw std::runtime_error("NGX command list unavailable");
    // Same authenticated wrapper interface used by ResTrack::PrepareSubmission,
    // without installing queue hooks or emitting a submission/fence operation.
    static constexpr IID nativeListIid = {
        0xadec44e2, 0x61f0, 0x45c3, { 0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff }
    };
    ComPtr<IUnknown> native, identity;
    IUnknown* object = list;
    if (SUCCEEDED(list->QueryInterface(nativeListIid, reinterpret_cast<void**>(native.GetAddressOf()))) && native)
        object = native.Get();
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))))
        throw std::runtime_error("NGX command-list COM identity unavailable");
    return identity;
}

Json OwnEndpointResource(EndpointTrace& trace, ID3D12Resource* resource)
{
    if (!resource)
        return nullptr;
    ComPtr<IUnknown> identity;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity))))
        throw std::runtime_error("NGX resource COM identity unavailable");
    const auto desc = resource->GetDesc();
    if (std::none_of(trace.resources.begin(), trace.resources.end(),
                     [&](const auto& owned) { return owned.Get() == identity.Get(); }))
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
            throw std::runtime_error("NGX resource device unavailable");
        const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (trace.resources.size() >= 1 + 2 * MaxNgxEndpoints || bytes > MaxEndpointResourceBytes ||
            trace.resourceBytes > MaxEndpointResourceBytes - bytes)
            throw std::runtime_error("endpoint resource ownership budget exhausted");
        trace.resources.push_back(identity);
        trace.resourceBytes += bytes;
    }
    return { { "address", std::format("{:x}", uintptr_t(resource)) },
        { "canonical_identity", std::format("{:x}", uintptr_t(identity.Get())) },
        { "matches_fog_resource", !trace.resources.empty() && identity.Get() == trace.resources.front().Get() },
        { "description", { { "dimension", UINT(desc.Dimension) }, { "alignment", desc.Alignment },
            { "width", desc.Width }, { "height", desc.Height }, { "depth_or_array_size", desc.DepthOrArraySize },
            { "mip_levels", desc.MipLevels }, { "format", UINT(desc.Format) },
            { "sample_count", desc.SampleDesc.Count }, { "sample_quality", desc.SampleDesc.Quality },
            { "layout", UINT(desc.Layout) }, { "flags", UINT(desc.Flags) } } } };
}

HRESULT WINAPI HookCreateList(ID3D12Device* device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                               ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial,
                               REFIID iid, void** result)
{
    const auto hr = originalCreateList(device, nodeMask, type, allocator, initial, iid, result);
    if (SUCCEEDED(hr) && result && *result)
        Track([&] {
            ComPtr<ID3D12GraphicsCommandList> list;
            if (FAILED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&list))))
                return;
            const auto identity = ListIdentity(list.Get());
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            // Reused COM addresses must not inherit a previous object's recording.
            // New lists stay ineligible until an explicitly observed Reset. The
            // newer CreateCommandList1 API creates closed lists and requires Reset.
            data.lists.erase(identity.Get());
        });
    return hr;
}

template <typename Fn> void TrackList(ID3D12GraphicsCommandList* list, Fn&& update) noexcept
{
    Track([&] {
        const auto identity = ListIdentity(list);
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        if (auto found = data.lists.find(identity.Get()); found != data.lists.end())
            update(found->second);
    });
}
HRESULT WINAPI HookReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                          ID3D12PipelineState* initial)
{
    const HRESULT hr = originalReset(list, allocator, initial);
    if (SUCCEEDED(hr))
        Track([&] {
            const auto identity = ListIdentity(list);
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            if (!data.lists.contains(identity.Get()) && data.lists.size() >= MaxCommandLists)
                throw std::runtime_error("command-list provenance budget exhausted");
            auto& state = data.lists[identity.Get()];
            state = {};
            state.generation = ++data.nextRecording;
            state.known = true;
        });
    return hr;
}
void WINAPI HookClearState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* initial)
{
    TrackList(list, [](auto& state) { state.known = false; });
    originalClearState(list, initial);
}
void WINAPI HookSetPredication(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, UINT64 offset,
                                D3D12_PREDICATION_OP operation)
{
    TrackList(list, [&](auto& state) { state.predicated = buffer != nullptr; });
    originalSetPredication(list, buffer, offset, operation);
}
void WINAPI HookBeginQuery(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
{
    TrackList(list, [&](auto& state) {
        if (state.queryCount >= state.queries.size())
            state.known = false;
        else
            state.queries[state.queryCount++] = { heap, type, index };
    });
    originalBeginQuery(list, heap, type, index);
}
void WINAPI HookEndQuery(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
{
    if (type != D3D12_QUERY_TYPE_TIMESTAMP) // Timestamps have no BeginQuery.
        TrackList(list, [&](auto& state) {
            for (UINT i = 0; i < state.queryCount; ++i)
                if (state.queries[i].heap == heap && state.queries[i].type == type && state.queries[i].index == index)
                {
                    state.queries[i] = state.queries[--state.queryCount];
                    return;
                }
            state.known = false;
        });
    originalEndQuery(list, heap, type, index);
}
void WINAPI HookExecuteBundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle)
{
    TrackList(list, [](auto& state) { state.known = false; });
    originalExecuteBundle(list, bundle);
}
void WINAPI HookSetViewports(ID3D12GraphicsCommandList* list, UINT count, const D3D12_VIEWPORT* viewports)
{
    TrackList(list, [&](auto& state) {
        if (count > state.viewports.size() || (count && !viewports))
            state.known = false;
        else
        {
            state.viewportCount = count;
            if (count) std::copy_n(viewports, count, state.viewports.begin());
        }
    });
    originalSetViewports(list, count, viewports);
}
void WINAPI HookSetScissors(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RECT* scissors)
{
    TrackList(list, [&](auto& state) {
        if (count > state.scissors.size() || (count && !scissors))
            state.known = false;
        else
        {
            state.scissorCount = count;
            if (count) std::copy_n(scissors, count, state.scissors.begin());
        }
    });
    originalSetScissors(list, count, scissors);
}
void WINAPI HookBeginRenderPass(ID3D12GraphicsCommandList4* list, UINT count,
                                const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                                const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags)
{
    TrackList(list, [](auto& state) { state.renderPass = true; });
    originalBeginRenderPass(list, count, targets, depth, flags);
}
void WINAPI HookEndRenderPass(ID3D12GraphicsCommandList4* list)
{
    TrackList(list, [](auto& state) {
        if (!state.renderPass) state.known = false;
        state.renderPass = false;
    });
    originalEndRenderPass(list);
}

void PollRearm() noexcept
{
    static std::atomic<ULONGLONG> nextPoll { 0 };
    const auto now = GetTickCount64();
    auto due = nextPoll.load();
    if (now < due || !nextPoll.compare_exchange_strong(due, now + 1000))
        return;
    Metadata([] {
        static std::mutex requestMutex;
        std::unique_lock lock(requestMutex, std::try_to_lock);
        if (!lock)
            return;
        if (captureEnabled.load() && captureTrackingValid.load())
        {
            const auto captureRequest = Util::ExePath().parent_path() / L"FSRRR-fog-capture.request";
            const auto captureAttributes = GetFileAttributesW(captureRequest.c_str());
            if (captureAttributes != INVALID_FILE_ATTRIBUTES && !(captureAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                FSRDFogLayerCapture::Request())
            {
                if (DeleteFileW(captureRequest.c_str()))
                    LOG_INFO("[FSRRR fog capture] explicit one-shot request queued; waiting for fully authenticated draw/state");
                else
                    FSRDFogLayerCapture::CancelRequest();
            }
        }
        if (arm.load() >= MaxRearms)
            return;
        const auto request = Util::ExePath().parent_path() / L"FSRRR-fog-probe.request";
        const auto attributes = GetFileAttributesW(request.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return;
        // Consume only this explicit diagnostic marker. Failure leaves the request
        // pending; no game configuration, output, or existing capture is removed.
        if (!DeleteFileW(request.c_str()))
            return;
        const unsigned generation = arm.fetch_add(1) + 1;
        drawLogs.store(0);
        emptyLogs.store(0);
        LOG_INFO("[FSRRR fog probe] re-armed metadata logs arm={}/{}; global scope sequence unchanged; not a frame association",
                 generation, MaxRearms);
    });
}

void __fastcall HookFogNode(void* node, void* context)
{
    PollRearm();
    Scope current { scope, scopes.fetch_add(1) + 1, node, context };
    scope = &current;
    struct RestoreScope
    {
        Scope* previous;
        ~RestoreScope() { scope = previous; }
    } restore { current.previous };
    originalFogNode(node, context);
    if (!current.draws && emptyLogs.fetch_add(1) < 4)
        Metadata([&] { LOG_INFO("[FSRRR fog probe] scope={} thread={} node={:x} context={:x}: no scoped D3D draw observed",
                               current.serial, GetCurrentThreadId(), uintptr_t(node), uintptr_t(context)); });
}

void WINAPI HookSetPso(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    if (scope && !inMetadata)
    {
        scope->psoList = list;
        scope->pso = pso;
    }
    originalSetPso(list, pso);
}

void WINAPI HookSetRtv(ID3D12GraphicsCommandList* list, UINT count,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL contiguous,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    if (scope && !inMetadata)
    {
        scope->rtvList = list;
        scope->rtvCount = count;
        scope->contiguous = contiguous;
        scope->rtvs.fill({});
        const UINT handles = contiguous ? (count ? 1 : 0) : count;
        if (rtvs && handles <= scope->rtvs.size())
            std::copy_n(rtvs, handles, scope->rtvs.begin());
        else if (handles)
            scope->rtvList = nullptr; // Unknown/invalid call: do not invent views.
        scope->hasDsv = dsv != nullptr;
        scope->dsv = dsv ? *dsv : D3D12_CPU_DESCRIPTOR_HANDLE {};
        scope->boundRtv = {};
        if (count == 1 && rtvs && !dsv && captureEnabled.load() && FSRDFogLayerCapture::WantsCapture())
            Track([&] {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                RtvHeap* heap = nullptr;
                UINT index = 0;
                const auto* slot = FindRtv(data, rtvs[0].ptr, &heap, &index);
                if (!slot || !slot->known || !slot->resource)
                    return;
                auto& bound = scope->boundRtv;
                // RTV descriptors are consumed HERE, not at Draw. Ownership must
                // be acquired while this original bind's resource is API-valid;
                // the CPU descriptor may legally be recycled once the call returns.
                bound.resource = slot->resource;
                bound.heap = heap->heap;
                bound.view = slot->view;
                bound.heapGeneration = heap->generation;
                bound.slotGeneration = slot->generation;
                bound.index = index;
                bound.known = true;
            });
    }
    originalSetRtv(list, count, rtvs, contiguous, dsv);
}

void LogDraw(ID3D12GraphicsCommandList* list, bool indexed, UINT count, UINT instances,
             UINT start, INT baseVertex, UINT firstInstance) noexcept
{
    if (!scope || inMetadata)
        return;
    ++scope->draws;
    if (drawLogs.fetch_add(1) >= MaxDrawLogs)
        return;
    Metadata([&] {
        const auto& s = *scope;
        const bool knownPso = s.psoList == list && s.pso;
        const bool knownRtv = s.rtvList == list;
        const char* shader = "unknown/not authenticated";
        if (knownPso)
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (const auto& item : data.tagged)
                if (item.pso.Get() == s.pso)
                    shader = item.shader;
        }
        Json handles = Json::array();
        if (knownRtv)
            for (UINT i = 0; i < (s.contiguous ? (s.rtvCount ? 1 : 0) : s.rtvCount); ++i)
                handles.push_back(s.rtvs[i].ptr);
        Json record = {
            { "scope", s.serial }, { "arm", arm.load() }, { "thread", GetCurrentThreadId() }, { "node", uintptr_t(s.node) },
            { "context", uintptr_t(s.context) }, { "command_list", uintptr_t(list) },
            { "indexed", indexed }, { "count", count }, { "instances", instances }, { "start", start },
            { "base_vertex", baseVertex }, { "first_instance", firstInstance },
            { "pso_observed_in_scope", knownPso }, { "pso", knownPso ? uintptr_t(s.pso) : 0 },
            { "authenticated_shader", shader }, { "rtv_observed_in_scope", knownRtv },
            { "rtv_count", knownRtv ? s.rtvCount : 0 }, { "rtv_handles", handles },
            { "rtv_contiguous", knownRtv && s.contiguous }, { "has_dsv", knownRtv && s.hasDsv },
            { "dsv_handle", knownRtv && s.hasDsv ? s.dsv.ptr : 0 },
        };
        LOG_INFO("[FSRRR fog probe] draw {}", record.dump());
    });
}

struct CapturePlan
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> main;
    ComPtr<ID3D12PipelineState> originalPso, authoredPso;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> sourceHeap, privateHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE originalRtv {}, frozenOriginalRtv {}, authoredRtv {};
    D3D12_RENDER_TARGET_VIEW_DESC originalView {};
    BOOL contiguous = FALSE;
    FSRDFogLayerCapture::Layers layers;
    Json provenance;
    std::shared_ptr<EndpointTrace> endpoint;
};

void RefuseCapture(const char* reason)
{
    if (captureRefusalLogs.fetch_add(1) < 8)
        LOG_WARN("[FSRRR fog capture] pending request not captured: {}", reason);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after, 0);
    list->ResourceBarrier(1, &barrier);
}

void CopyMain(ID3D12GraphicsCommandList* list, const CapturePlan& plan, ID3D12Resource* destination)
{
    // A valid original draw requires this exact selected RTV subresource in RT
    // state. Only mip0/slice0 is supported, and all original state is restored.
    Transition(list, plan.main.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    struct RestoreMainState
    {
        ID3D12GraphicsCommandList* list;
        ID3D12Resource* resource;
        ~RestoreMainState()
        {
            Transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    } restore { list, plan.main.Get() };
    const CD3DX12_TEXTURE_COPY_LOCATION source(plan.main.Get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION target(destination, 0);
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
}

std::shared_ptr<CapturePlan> PrepareCapture(ID3D12GraphicsCommandList* list, UINT count, UINT instances,
                                           UINT start, UINT firstInstance)
{
    if (!captureEnabled.load() || !captureTrackingValid.load() || captureStarted.load() ||
        !FSRDFogLayerCapture::WantsCapture() || !scope)
        return {};
    const auto& s = *scope;
    if (count != 3 || instances != 1 || start != 0 || firstInstance != 0 ||
        list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || s.psoList != list || !s.pso ||
        s.rtvList != list || s.rtvCount != 1 || s.hasDsv)
    {
        RefuseCapture("draw, in-scope PSO, or exact single RTV not observed");
        return {};
    }
    auto plan = std::make_shared<CapturePlan>();
    plan->endpoint = std::make_shared<EndpointTrace>();
    ListState state;
    {
        const auto identity = ListIdentity(list);
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        const auto foundState = data.lists.find(identity.Get());
        if (foundState == data.lists.end() || !foundState->second.known || foundState->second.predicated ||
            foundState->second.renderPass || foundState->second.queryCount ||
            foundState->second.viewportCount != 1 || foundState->second.scissorCount != 1)
        {
            RefuseCapture("state history since Reset incomplete, or query/predication/render-pass/bundle/viewport guard");
            return {};
        }
        state = foundState->second;
        const auto foundPso = std::find_if(data.tagged.begin(), data.tagged.end(),
                                          [&](const auto& item) { return item.pso.Get() == s.pso; });
        if (foundPso == data.tagged.end() || !foundPso->authored)
        {
            RefuseCapture("authenticated side-effect-free original/private PSO pair unavailable");
            return {};
        }
        const auto& bound = s.boundRtv;
        if (!bound.known || !bound.resource || bound.view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            bound.view.ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D || bound.view.Texture2D.MipSlice != 0 ||
            bound.view.Texture2D.PlaneSlice != 0)
        {
            RefuseCapture("exact explicit mip0 RGBA16F RTV descriptor provenance unavailable");
            return {};
        }
        // Already owned since the original OM bind; do not resolve its CPU handle
        // again. The game may legally have reused that descriptor in the meantime.
        plan->main = bound.resource;
        plan->sourceHeap = bound.heap;
        plan->originalView = bound.view;
        plan->originalPso = foundPso->pso;
        plan->authoredPso = foundPso->authored;
        plan->root = foundPso->root;
        plan->endpoint->list = identity;
        plan->endpoint->generation = state.generation;
        plan->endpoint->ordinal = ++foundState->second.endpointOrdinal;
        const auto& b = foundPso->blend;
        plan->provenance = {
            { "schema", "optiscaler.fsr_rr.fog_draw.v1" }, { "scope_serial", s.serial }, { "scope_arm", arm.load() },
            { "command_list", std::format("{:x}", uintptr_t(list)) }, { "command_list_generation", state.generation },
            { "main_resource_address", std::format("{:x}", uintptr_t(plan->main.Get())) },
            { "original_pso", std::format("{:x}", uintptr_t(plan->originalPso.Get())) },
            { "authored_pso", std::format("{:x}", uintptr_t(plan->authoredPso.Get())) },
            { "source_rtv", { { "cpu_handle", s.rtvs[0].ptr }, { "heap_address", uintptr_t(bound.heap.Get()) },
                { "heap_generation", bound.heapGeneration }, { "slot", bound.index }, { "slot_generation", bound.slotGeneration },
                { "format", UINT(bound.view.Format) }, { "mip_slice", 0 }, { "array_slice", 0 },
                { "ownership_acquired", "original OMSetRenderTargets" } } },
            { "shader", { { "ps_sha256", foundPso->pixelSha256 }, { "vs_sha256", FogVertexSha256 } } },
            { "draw", { { "vertex_count", count }, { "instance_count", instances },
                { "start_vertex", start }, { "start_instance", firstInstance } } },
            { "original_blend", { { "src", UINT(b.SrcBlend) }, { "dst", UINT(b.DestBlend) },
                { "enabled", bool(b.BlendEnable) }, { "logic_op_enabled", bool(b.LogicOpEnable) },
                { "op", UINT(b.BlendOp) }, { "src_alpha", UINT(b.SrcBlendAlpha) },
                { "dst_alpha", UINT(b.DestBlendAlpha) }, { "op_alpha", UINT(b.BlendOpAlpha) },
                { "write_mask", UINT(b.RenderTargetWriteMask) } } },
            { "rr_frame_association", "not_established" },
            { "private_clear", { 0, 0, 0, 0 } }, { "root_bindings_viewport_scissor", "inherited unchanged" },
        };
    }
    const auto desc = plan->main->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
        (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS) ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || !desc.Width || !desc.Height ||
        desc.Width > 8192 || desc.Height > 8192)
    {
        RefuseCapture("RTV resource is not a supported single-array/single-sample RGBA16F target");
        return {};
    }
    if (captureStarted.exchange(true))
        return {};
    if (FAILED(plan->main->GetDevice(IID_PPV_ARGS(&plan->device))))
        throw std::runtime_error("fog target device unavailable");
    ComPtr<ID3D12Device> listDevice;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice))) || listDevice.Get() != plan->device.Get())
        throw std::runtime_error("fog target/list device mismatch");

    plan->originalRtv = s.rtvs[0];
    plan->contiguous = s.contiguous;
    const auto& v = state.viewports[0];
    const auto& r = state.scissors[0];
    plan->provenance["viewports"] = { { v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth } };
    plan->provenance["scissor_rects"] = { { r.left, r.top, r.right, r.bottom } };
    plan->provenance["source_rtv"]["width"] = desc.Width;
    plan->provenance["source_rtv"]["height"] = desc.Height;

    auto beforeDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, desc.Width, desc.Height, 1, 1);
    auto authoredDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, desc.Width, desc.Height, 1, 1,
                                                     1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto mainBytes = plan->device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    const auto copyBytes = plan->device->GetResourceAllocationInfo(0, 1, &beforeDesc).SizeInBytes;
    const auto authoredBytes = plan->device->GetResourceAllocationInfo(0, 1, &authoredDesc).SizeInBytes;
    if (mainBytes > MaxCaptureTextureBytes || copyBytes > MaxCaptureTextureBytes || authoredBytes > MaxCaptureTextureBytes ||
        mainBytes + 2 * copyBytes + authoredBytes > MaxCaptureTextureBytes)
        throw std::runtime_error("fog capture retained texture allocation exceeds 256 MiB");
    plan->endpoint->fog = {
        { "scope_serial", plan->provenance["scope_serial"] },
        { "command_list_identity", std::format("{:x}", uintptr_t(plan->endpoint->list.Get())) },
        { "command_list_generation", plan->endpoint->generation }, { "endpoint_ordinal", plan->endpoint->ordinal },
        { "ordinal_scope", "observed endpoints within this Reset recording only" },
        { "resource", OwnEndpointResource(*plan->endpoint, plan->main.Get()) },
        { "subresource", 0 }, { "view_format", UINT(plan->originalView.Format) },
        { "rr_frame_association", "not_established" }
    };
    plan->provenance["endpoint_origin"] = plan->endpoint->fog;
    const CD3DX12_HEAP_PROPERTIES properties(D3D12_HEAP_TYPE_DEFAULT);
    auto allocate = [&](const D3D12_RESOURCE_DESC& textureDesc, D3D12_RESOURCE_STATES initial,
                         FSRDFogLayerCapture::Texture& output) {
        if (FAILED(plan->device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &textureDesc, initial,
                                                         nullptr, IID_PPV_ARGS(&output.resource))))
            throw std::runtime_error("private fog capture texture allocation failed");
        output.viewFormat = textureDesc.Format;
        output.state = initial;
    };
    allocate(beforeDesc, D3D12_RESOURCE_STATE_COPY_DEST, plan->layers.before);
    allocate(beforeDesc, D3D12_RESOURCE_STATE_COPY_DEST, plan->layers.after);
    allocate(authoredDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, plan->layers.authored);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 2;
    if (FAILED(plan->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&plan->privateHeap))))
        throw std::runtime_error("private fog RTV heap allocation failed");
    plan->authoredRtv = plan->privateHeap->GetCPUDescriptorHandleForHeapStart();
    plan->frozenOriginalRtv = plan->authoredRtv;
    plan->frozenOriginalRtv.ptr += plan->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC view {};
    view.Format = authoredDesc.Format;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    plan->device->CreateRenderTargetView(plan->layers.authored.resource.Get(), &view, plan->authoredRtv);
    plan->device->CreateRenderTargetView(plan->main.Get(), &plan->originalView, plan->frozenOriginalRtv);
    plan->provenance["restoration"] = { { "frozen_rtv_handle", plan->frozenOriginalRtv.ptr },
        { "binding", "exact owned original resource/view; original CPU descriptor may have been reused" } };

    // Retain BEFORE the first private-copy command. This independent ticket keeps
    // earlier work alive even if the later readback helper refuses or throws.
    FSRDSubmission::Retain(plan->device.Get(), list, plan);
    CopyMain(list, *plan, plan->layers.before.resource.Get());
    return plan;
}

void PublishFogEndpoint(const std::shared_ptr<CapturePlan>& plan) noexcept
{
    Metadata([&] {
        auto& data = Data();
        {
            std::lock_guard lock(data.mutex);
            data.endpoint = plan->endpoint;
            endpointActive.store(true, std::memory_order_release);
        }
        LOG_INFO("[FSRRR fog endpoint] original fog draw recorded; origin={}; next {} CPU-observed RR endpoints only; no GPU/frame/content association",
                 plan->endpoint->fog.dump(), MaxNgxEndpoints);
    });
}

void FinishCapture(ID3D12GraphicsCommandList* list, const std::shared_ptr<CapturePlan>& plan)
{
    if (!plan)
        return;
    CopyMain(list, *plan, plan->layers.after.resource.Get());
    const FLOAT clear[] = { 0, 0, 0, 0 }; // Authenticated fog PS may discard: untouched pixels are identity.
    list->ClearRenderTargetView(plan->authoredRtv, clear, 0, nullptr);
    {
        struct RestoreDrawState
        {
            ID3D12GraphicsCommandList* list;
            const CapturePlan& plan;
            ~RestoreDrawState()
            {
                originalSetPso(list, plan.originalPso.Get());
                originalSetRtv(list, 1, &plan.frozenOriginalRtv, plan.contiguous, nullptr);
            }
        } restore { list, *plan };
        // Do not touch roots, descriptor heaps, viewport/scissor or depth state.
        originalSetPso(list, plan->authoredPso.Get());
        originalSetRtv(list, 1, &plan->authoredRtv, FALSE, nullptr);
        originalDraw(list, 3, 1, 0, 0); // Separate private-layer instance, never a second original-target draw.
    }
    const bool recorded = FSRDFogLayerCapture::Record(plan->device.Get(), list, plan->layers,
                                                     plan->provenance.dump(), plan);
    LOG_INFO("[FSRRR fog capture] original draw preserved; private readback recorded={} scope={}",
             recorded, plan->provenance["scope_serial"].get<uint64_t>());
}

void WINAPI HookDraw(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT start, UINT firstInstance)
{
    LogDraw(list, false, count, instances, start, 0, firstInstance);
    std::shared_ptr<CapturePlan> plan;
    if (captureEnabled.load() && !inMetadata && scope)
        Metadata([&] {
            try { plan = PrepareCapture(list, count, instances, start, firstInstance); }
            catch (const std::exception& error)
            {
                captureStarted.store(true);
                FSRDFogLayerCapture::CancelRequest();
                LOG_WARN("[FSRRR fog capture] preparation stopped: {}", error.what());
            }
        });
    originalDraw(list, count, instances, start, firstInstance);
    if (plan)
    {
        PublishFogEndpoint(plan); // Publish only AFTER the original target draw was recorded once.
        Metadata([&] {
            try { FinishCapture(list, plan); }
            catch (const std::exception& error)
            {
                FSRDFogLayerCapture::CancelRequest();
                LOG_WARN("[FSRRR fog capture] post-draw capture stopped; earlier GPU storage retained: {}", error.what());
            }
        });
    }
}
void WINAPI HookDrawIndexed(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT start,
                            INT baseVertex, UINT firstInstance)
{
    LogDraw(list, true, count, instances, start, baseVertex, firstInstance);
    originalDrawIndexed(list, count, instances, start, baseVertex, firstInstance);
}

void PrepareAuthoredPso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, TaggedPso& tagged, bool streamSafe)
{
    if (!captureEnabled.load() || !streamSafe || !desc.pRootSignature || !desc.VS.pShaderBytecode ||
        desc.VS.BytecodeLength != FogVertexBytes || desc.GS.BytecodeLength || desc.HS.BytecodeLength || desc.DS.BytecodeLength ||
        desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides || desc.InputLayout.NumElements ||
        desc.NumRenderTargets != 1 || desc.RTVFormats[0] != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 || desc.DepthStencilState.DepthEnable ||
        desc.DepthStencilState.StencilEnable || desc.BlendState.AlphaToCoverageEnable ||
        desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE || desc.Flags != D3D12_PIPELINE_STATE_FLAG_NONE)
        return;
    const auto& blend = desc.BlendState.RenderTarget[0];
    if (!blend.BlendEnable || blend.LogicOpEnable || blend.SrcBlend != D3D12_BLEND_ONE ||
        blend.DestBlend != D3D12_BLEND_INV_SRC_ALPHA || blend.BlendOp != D3D12_BLEND_OP_ADD ||
        blend.SrcBlendAlpha != D3D12_BLEND_ONE || blend.DestBlendAlpha != D3D12_BLEND_INV_SRC_ALPHA ||
        blend.BlendOpAlpha != D3D12_BLEND_OP_ADD || blend.RenderTargetWriteMask != D3D12_COLOR_WRITE_ENABLE_ALL)
        return;
    Hash hash(Data().crypto);
    hash.Add(desc.VS.pShaderBytecode, ULONG(desc.VS.BytecodeLength));
    if (hash.Finish() != FogVertexSha256)
        return;
    // These exact authenticated VS/PS binaries contain only read-only bindings,
    // arithmetic, output stores and (for some PS variants) discard. No UAV/depth/
    // coverage writes. Never substitute a generic full-screen vertex shader.
    tagged.vertexBytes.assign(static_cast<const BYTE*>(desc.VS.pShaderBytecode),
                               static_cast<const BYTE*>(desc.VS.pShaderBytecode) + desc.VS.BytecodeLength);
    tagged.pixelBytes.assign(static_cast<const BYTE*>(desc.PS.pShaderBytecode),
                              static_cast<const BYTE*>(desc.PS.pShaderBytecode) + desc.PS.BytecodeLength);
    tagged.root = desc.pRootSignature;
    tagged.blend = blend;
    auto clone = desc;
    clone.pRootSignature = tagged.root.Get();
    clone.VS = { tagged.vertexBytes.data(), tagged.vertexBytes.size() };
    clone.PS = { tagged.pixelBytes.data(), tagged.pixelBytes.size() };
    clone.CachedPSO = {}; // Original format/blend cache is incompatible with the private PSO.
    clone.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    clone.BlendState.RenderTarget[0].BlendEnable = FALSE;
    clone.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    clone.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(tagged.pso->GetDevice(IID_PPV_ARGS(&device))))
    {
        const auto hr = originalCreateGraphics(device.Get(), &clone, IID_PPV_ARGS(&tagged.authored));
        LOG_INFO("[FSRRR fog capture] exact shader private PSO preparation result={:x}; original={:x} private={:x}",
                 UINT(hr), uintptr_t(tagged.pso.Get()), uintptr_t(tagged.authored.Get()));
    }
}

void RecordPso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3D12PipelineState* pso, const char* path,
                bool streamSafe = true)
{
    if (!pso || !desc.PS.pShaderBytecode)
        return;
    auto& data = Data();
    for (const auto& identity : FogShaders)
    {
        if (desc.PS.BytecodeLength != identity.bytes)
            continue;
        Hash hash(data.crypto);
        hash.Add(desc.PS.pShaderBytecode, ULONG(desc.PS.BytecodeLength));
        if (hash.Finish() != identity.sha256)
            continue;
        std::lock_guard lock(data.mutex);
        if (data.tagged.size() >= MaxPsoLogs)
            return;
        for (const auto& item : data.tagged)
            if (item.pso.Get() == pso)
                return;
        TaggedPso tagged { pso, identity.name };
        tagged.pixelSha256 = identity.sha256;
        PrepareAuthoredPso(desc, tagged, streamSafe);
        data.tagged.push_back(std::move(tagged));
        Json targets = Json::array();
        for (UINT i = 0; i < std::min(desc.NumRenderTargets, UINT(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)); ++i)
        {
            // Inactive descriptors need not be initialized by the caller. When
            // independent blending is off, only RT0's blend equation is defined.
            const UINT blendIndex = desc.BlendState.IndependentBlendEnable ? i : 0;
            const auto& b = desc.BlendState.RenderTarget[blendIndex];
            targets.push_back({ { "index", i }, { "format", UINT(desc.RTVFormats[i]) },
                { "blend_descriptor_index", blendIndex },
                { "blend_enable", bool(b.BlendEnable) }, { "logic_enable", bool(b.LogicOpEnable) },
                { "src", UINT(b.SrcBlend) }, { "dst", UINT(b.DestBlend) }, { "op", UINT(b.BlendOp) },
                { "src_alpha", UINT(b.SrcBlendAlpha) }, { "dst_alpha", UINT(b.DestBlendAlpha) },
                { "op_alpha", UINT(b.BlendOpAlpha) }, { "logic_op", UINT(b.LogicOp) },
                { "write_mask", b.RenderTargetWriteMask } });
        }
        Json record = { { "pso", uintptr_t(pso) }, { "creation_path", path }, { "shader", identity.name },
            { "ps_sha256", identity.sha256 }, { "ps_bytes", desc.PS.BytecodeLength },
            { "capture_shader_pair_ready", bool(data.tagged.back().authored) },
            { "private_pso", uintptr_t(data.tagged.back().authored.Get()) },
            { "root_signature", uintptr_t(desc.pRootSignature) },
            { "independent_blend", bool(desc.BlendState.IndependentBlendEnable) },
            { "alpha_to_coverage", bool(desc.BlendState.AlphaToCoverageEnable) },
            { "num_render_targets", desc.NumRenderTargets }, { "targets", targets },
            { "sample_count", desc.SampleDesc.Count }, { "sample_quality", desc.SampleDesc.Quality },
            { "sample_mask", desc.SampleMask }, { "topology_type", UINT(desc.PrimitiveTopologyType) },
            { "dsv_format", UINT(desc.DSVFormat) }, { "depth_enable", bool(desc.DepthStencilState.DepthEnable) },
            { "depth_func", UINT(desc.DepthStencilState.DepthFunc) },
            { "depth_write_mask", UINT(desc.DepthStencilState.DepthWriteMask) },
            { "cached_pso_bytes", desc.CachedPSO.CachedBlobSizeInBytes } };
        LOG_INFO("[FSRRR fog probe] authenticated PSO {}", record.dump());
        return;
    }
}

HRESULT WINAPI HookCreateGraphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                   REFIID iid, void** result)
{
    const HRESULT hr = originalCreateGraphics(device, desc, iid, result);
    if (SUCCEEDED(hr) && desc && result && *result)
        Metadata([&] {
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&pso))))
                RecordPso(*desc, pso.Get(), "CreateGraphicsPipelineState");
        });
    return hr;
}
HRESULT WINAPI HookCreateStream(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                 REFIID iid, void** result)
{
    const HRESULT hr = originalCreateStream(device, desc, iid, result);
    if (SUCCEEDED(hr) && desc && result && *result)
        Metadata([&] {
            CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER parsed;
            // The existing parser rejects unknown subobjects; do not guess their layout.
            if (FAILED(D3DX12ParsePipelineStream(*desc, &parsed)))
                return;
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&pso))))
            {
                const auto viewInstancing = D3D12_VIEW_INSTANCING_DESC(parsed.PipelineStream.ViewInstancingDesc);
                const auto depth = D3D12_DEPTH_STENCIL_DESC1(parsed.PipelineStream.DepthStencilState);
                RecordPso(parsed.PipelineStream.GraphicsDescV0(), pso.Get(), "CreatePipelineState",
                           viewInstancing.ViewInstanceCount == 0 && !depth.DepthBoundsTestEnable);
            }
        });
    return hr;
}

template <typename T> bool ReadLocal(uintptr_t address, T& result)
{
    SIZE_T bytes = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &result, sizeof(result),
                             &bytes) && bytes == sizeof(result);
}

bool Authenticate(uintptr_t& entry)
{
    const auto path = Util::ExePath();
    if (_wcsicmp(path.filename().c_str(), L"Cyberpunk2077.exe") != 0)
        return false;
    version_t file, product;
    if (!Util::GetFileVersion(path.wstring(), &file, &product) || file != version_t(3, 0, 80, 51928) ||
        product != version_t(2, 3, 1, 0))
        return false;
    HANDLE fileHandle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
        return false;
    struct CloseFile { HANDLE handle; ~CloseFile() { CloseHandle(handle); } } close { fileHandle };
    Hash hash(Data().crypto);
    std::array<unsigned char, 65536> buffer;
    DWORD read = 0;
    for (;;)
    {
        if (!ReadFile(fileHandle, buffer.data(), DWORD(buffer.size()), &read, nullptr))
            return false;
        if (!read)
            break;
        hash.Add(buffer.data(), read);
    }
    if (hash.Finish() != ExeSha256)
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS64 nt {};
    if (!base || !ReadLocal(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew != 0x1d0 ||
        !ReadLocal(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.TimeDateStamp != 0x68af45ea ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != 0x04efc000)
        return false;
    entry = base + FogNodeRva;
    std::array<unsigned char, FogPrologue.size()> prologue {};
    MEMORY_BASIC_INFORMATION memory {};
    if (!ReadLocal(entry, prologue) || prologue != FogPrologue ||
        !VirtualQuery(reinterpret_cast<void*>(entry), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != reinterpret_cast<void*>(base) ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        (memory.Protect & PAGE_GUARD))
        return false;
    return true;
}
} // namespace

void ObserveNgxInput(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
                     ID3D12Resource* colorBeforeParticles, uint64_t featureId, uint64_t frameIndex,
                     UINT renderWidth, UINT renderHeight) noexcept
{
    if (!endpointActive.load(std::memory_order_acquire))
        return; // No COM calls, allocation, file polling or locks on the inactive path.
    Metadata([&] {
        std::shared_ptr<EndpointTrace> trace;
        try
        {
            const auto identity = EndpointListIdentity(list);
            Json record;
            auto& data = Data();
            {
                std::lock_guard lock(data.mutex);
                trace = data.endpoint;
                if (!trace || trace->count >= MaxNgxEndpoints)
                    return;
                const unsigned index = ++trace->count;
                const auto found = data.lists.find(identity.Get());
                const bool recordingKnown = captureTrackingValid.load() && found != data.lists.end();
                const uint64_t generation = recordingKnown ? found->second.generation : 0;
                const uint64_t ordinal = recordingKnown ? ++found->second.endpointOrdinal : 0;
                const bool sameList = identity.Get() == trace->list.Get();
                const bool sameRecording = recordingKnown && sameList && generation == trace->generation;
                const char* relation = !recordingKnown ? "unknown_recording" : !sameList ? "different_command_list" :
                    !sameRecording ? "different_Reset_recording" : ordinal > trace->ordinal ?
                    "same_recording_later_endpoint" : "same_recording_order_unestablished";
                record = {
                    { "schema", "optiscaler.fsr_rr.fog_ngx_endpoint.v1" },
                    { "endpoint_index", index }, { "fog_origin", trace->fog },
                    { "feature_id", featureId }, { "rr_frame_index", frameIndex },
                    { "render_extent", { renderWidth, renderHeight } },
                    { "command_list_address", std::format("{:x}", uintptr_t(list)) },
                    { "command_list_identity", std::format("{:x}", uintptr_t(identity.Get())) },
                    { "recording_known", recordingKnown },
                    { "command_list_generation", recordingKnown ? Json(generation) : Json(nullptr) },
                    { "endpoint_ordinal", recordingKnown ? Json(ordinal) : Json(nullptr) },
                    { "local_recording_relation", relation },
                    { "color", OwnEndpointResource(*trace, color) },
                    { "color_before_particles", OwnEndpointResource(*trace, colorBeforeParticles) },
                    { "ngx_view", "resource pointers only; mip/array/plane/subrect base not observed" },
                    { "rr_frame_association", "not_established" },
                    { "gpu_execution_order", "not_observed" }, { "intervening_writes", "not_tracked" },
                    { "unchanged_contents", "not_verified" },
                    { "observation_order", "CPU endpoint callbacks only; not submission or presentation order" }
                };
                if (trace->count == MaxNgxEndpoints)
                {
                    endpointActive.store(false, std::memory_order_release);
                    data.endpoint.reset(); // Local shared owners outlive the lock and final log.
                }
            }
            LOG_INFO("[FSRRR fog endpoint] NGX {}", record.dump());
        }
        catch (const std::exception& error)
        {
            endpointActive.store(false, std::memory_order_release);
            {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                trace = std::move(data.endpoint);
            }
            LOG_WARN("[FSRRR fog endpoint] observation window stopped; association incomplete: {}", error.what());
        }
    });
}

void Initialize(bool enabled)
{
    if (!enabled)
        return;
    std::call_once(initialization, [] {
        Metadata([] {
            uintptr_t entry = 0;
            if (!Authenticate(entry))
            {
                LOG_WARN("[FSRRR fog probe] refused: executable name/version/SHA256/live prologue authentication failed");
                return;
            }
            originalFogNode = reinterpret_cast<FogNode>(entry);
            LONG error = DetourTransactionBegin();
            if (error == NO_ERROR)
            {
                error = DetourUpdateThread(GetCurrentThread());
                if (error == NO_ERROR)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalFogNode), HookFogNode);
                if (error == NO_ERROR)
                    error = DetourTransactionCommit();
                else
                    DetourTransactionAbort();
            }
            if (error != NO_ERROR)
            {
                originalFogNode = nullptr;
                LOG_WARN("[FSRRR fog probe] engine hook failed: {}", error);
                return;
            }
            captureEnabled.store(Config::Instance()->FfxDenoiserCyberpunkFogCapture.value_or_default());
            active.store(true);
            LOG_INFO("[FSRRR fog probe] enabled metadata-only; authenticated Cyberpunk 2.31 file 3.0.80.51928 SHA256={} RVA={:x}; first {} draws per arm; pipeline-library loads not authenticated",
                     ExeSha256, FogNodeRva, MaxDrawLogs);
            if (captureEnabled.load())
                LOG_INFO("[FSRRR fog capture] opt-in enabled; no render capture until FSRRR-fog-capture.request and all guards pass");
        });
    });
}

void HookDevice(ID3D12Device* device)
{
    if (!active.load() || !device)
        return;
    std::lock_guard lock(hookMutex);
    if (originalCreateGraphics)
        return;
    auto** table = *reinterpret_cast<void***>(device);
    originalCreateGraphics = reinterpret_cast<CreateGraphics>(table[10]);
    if (captureEnabled.load())
    {
        originalCreateList = reinterpret_cast<CreateList>(table[12]);
        originalCreateHeap = reinterpret_cast<CreateHeap>(table[14]);
        originalCreateRtv = reinterpret_cast<CreateRtv>(table[20]);
        originalCopyDescriptors = reinterpret_cast<CopyDescriptors>(table[23]);
        originalCopyDescriptorsSimple = reinterpret_cast<CopyDescriptorsSimple>(table[24]);
    }
    ComPtr<ID3D12Device2> device2;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2))))
        originalCreateStream = reinterpret_cast<CreateStream>((*reinterpret_cast<void***>(device2.Get()))[47]);
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateGraphics), HookCreateGraphics);
        if (error == NO_ERROR && originalCreateStream)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateStream), HookCreateStream);
        if (error == NO_ERROR && originalCreateList)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateList), HookCreateList);
        if (error == NO_ERROR && originalCreateHeap)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateHeap), HookCreateHeap);
        if (error == NO_ERROR && originalCreateRtv)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateRtv), HookCreateRtv);
        if (error == NO_ERROR && originalCopyDescriptors)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCopyDescriptors), HookCopyDescriptors);
        if (error == NO_ERROR && originalCopyDescriptorsSimple)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCopyDescriptorsSimple), HookCopyDescriptorsSimple);
        if (error == NO_ERROR)
            error = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    if (error != NO_ERROR)
    {
        originalCreateGraphics = nullptr;
        originalCreateStream = nullptr;
        originalCreateList = nullptr;
        originalCreateHeap = nullptr;
        originalCreateRtv = nullptr;
        originalCopyDescriptors = nullptr;
        originalCopyDescriptorsSimple = nullptr;
        captureTrackingValid.store(false);
    }
    LOG_INFO("[FSRRR fog probe] graphics PSO metadata hooks result={}", error);
}

void HookCommandList(ID3D12GraphicsCommandList* list)
{
    if (!active.load() || !list)
        return;
    std::lock_guard lock(hookMutex);
    if (originalDraw)
        return;
    auto** table = *reinterpret_cast<void***>(list);
    originalSetPso = reinterpret_cast<SetPso>(table[25]);
    originalSetRtv = reinterpret_cast<SetRtv>(table[46]);
    originalDraw = reinterpret_cast<Draw>(table[12]);
    originalDrawIndexed = reinterpret_cast<DrawIndexed>(table[13]);
    if (captureEnabled.load())
    {
        originalReset = reinterpret_cast<Reset>(table[10]);
        originalClearState = reinterpret_cast<ClearState>(table[11]);
        originalSetViewports = reinterpret_cast<SetViewports>(table[21]);
        originalSetScissors = reinterpret_cast<SetScissors>(table[22]);
        originalExecuteBundle = reinterpret_cast<ExecuteBundle>(table[27]);
        originalBeginQuery = reinterpret_cast<BeginQuery>(table[52]);
        originalEndQuery = reinterpret_cast<EndQuery>(table[53]);
        originalSetPredication = reinterpret_cast<SetPredication>(table[55]);
        ComPtr<ID3D12GraphicsCommandList4> list4;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))))
        {
            auto** table4 = *reinterpret_cast<void***>(list4.Get());
            originalBeginRenderPass = reinterpret_cast<BeginRenderPass>(table4[68]);
            originalEndRenderPass = reinterpret_cast<EndRenderPass>(table4[69]);
        }
    }
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalSetPso), HookSetPso);
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalSetRtv), HookSetRtv);
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalDraw), HookDraw);
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalDrawIndexed), HookDrawIndexed);
        if (error == NO_ERROR && originalReset)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalReset), HookReset);
        if (error == NO_ERROR && originalClearState)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalClearState), HookClearState);
        if (error == NO_ERROR && originalSetViewports)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalSetViewports), HookSetViewports);
        if (error == NO_ERROR && originalSetScissors)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalSetScissors), HookSetScissors);
        if (error == NO_ERROR && originalExecuteBundle)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalExecuteBundle), HookExecuteBundle);
        if (error == NO_ERROR && originalBeginQuery)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalBeginQuery), HookBeginQuery);
        if (error == NO_ERROR && originalEndQuery)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalEndQuery), HookEndQuery);
        if (error == NO_ERROR && originalSetPredication)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalSetPredication), HookSetPredication);
        if (error == NO_ERROR && originalBeginRenderPass)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalBeginRenderPass), HookBeginRenderPass);
        if (error == NO_ERROR && originalEndRenderPass)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalEndRenderPass), HookEndRenderPass);
        if (error == NO_ERROR)
            error = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    if (error != NO_ERROR)
    {
        originalSetPso = nullptr;
        originalSetRtv = nullptr;
        originalDraw = nullptr;
        originalDrawIndexed = nullptr;
        originalReset = nullptr;
        originalClearState = nullptr;
        originalSetViewports = nullptr;
        originalSetScissors = nullptr;
        originalExecuteBundle = nullptr;
        originalBeginQuery = nullptr;
        originalEndQuery = nullptr;
        originalSetPredication = nullptr;
        originalBeginRenderPass = nullptr;
        originalEndRenderPass = nullptr;
        captureTrackingValid.store(false);
    }
    LOG_INFO("[FSRRR fog probe] command-list metadata hooks result={}", error);
}
} // namespace FSRDCyberpunkFogProbe

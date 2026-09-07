#include "pch.h"
#include "FSRDCyberpunkFogProbe.h"
#include "FSRDFogLayerCapture.h"
#include "FSRDCyberpunkEarlyGuides.h"
#include "FSRDCyberpunkEngineAccess.h"
#include "FSRDCyberpunkGuideMatrix.h"
#include "FSRDCyberpunkGuidePass.h"

#include <Util.h>
#include <resource_tracking/FSRDSubmission.h>
#include <hooks/Hook_Utils.h>
#include <hooks/StreamlineEvaluationProvenance.h>
#include <detours/detours.h>
#include <include/d3dx/d3dx12.h>
#include <json.hpp>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <intrin.h>

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
constexpr size_t MaxRtvHeaps = 512, MaxRtvSlots = 65536, MaxCommandLists = 2048;
constexpr size_t MaxCpuSrvHeaps = 128;
constexpr UINT64 MaxCpuSrvSlots = 262144, MaxCpuSrvBytes = 16ull * 1024 * 1024;
constexpr SIZE_T GuideShaderBytes = 5824;
constexpr std::string_view GuideShaderSha256 = "a4bcbce1667fb6f3e130a18482e6088e2835d0028582a52e5a4668f7fae1b7ee";
constexpr uintptr_t GBufferInitializerRva = 0x771000;
constexpr FSRD::CyberpunkEngineAccess::CodeRange ProducerCode[] = {
    { 0x771000, 0xb0, "a6a7d1a616bd2cf6d15c57564c45a0f791d2e5d255a2a4c901269ddc44ca65fb" },
    { 0x20ded8, 0xf5, "b1f86fee3715f5f65209c0f4a0bc51d0b2c57388047c81e483f65e2241008753" },
    { 0x7710b0, 0x8c, "c9b8d869757aa20666bc96fac0477f0b55c5aa3a1e7dcb85920f340e35f7ddb2" },
    { 0x7711c8, 0x41, "db8138c090761f534aadb90b66535164c94addc01561e614602626b9acbed32b" },
    { 0x1fa1f8, 0x3c, "718d57eb7d9b01daf4c57690874faffc238620e8a978e5840263508097098666" }
};
constexpr uintptr_t LightingNodeRva = 0x154610, FullscreenHelperRva = 0x20c954;
constexpr uintptr_t FinalLightingHelperReturnRva = 0x155e0c, NativeFullscreenDrawReturnRva = 0x20ccac;
constexpr FSRD::CyberpunkEngineAccess::CodeRange LightingCode[] = {
    { 0x154610, 0x1960, "3beb50773dd93b66302aef64a3facea1d9a46976867546abfb16dbc6661ca761" },
    { 0x20c954, 0x63, "dcc4dcd485f4318d8708181a715d6c4b26619a9c8523648525821a49dec92180" },
    { 0x20cc64, 0x58, "4833392e71c08768cbd9f77675121fa58bc2ff9f8fb27080386226d5cf13dca0" },
    { 0x2221f4, 0xc38, "a42b9e7ead94a67cd1a3dc7e405614ec4eeeadb9b955c0db9ea817f6a56b0562" },
    { 0x7711c8, 0x41, "db8138c090761f534aadb90b66535164c94addc01561e614602626b9acbed32b" },
    { 0x22c1f4, 0x37, "56bd6fdfea2f273df26d56c0aa62834d6ca739b6cf76b16a60ce0943ba618502" },
    { 0x1f22e4, 0x4dc, "f4a5782e0cead125409e02ce0ff209aa5468564dc286cd1403798a1bb18f8bfc" },
    { 0x1f3a6c, 0x2b1, "3d8ea951900012b9cb82212dc4d84a01312eac865475cb2e86501cdb138e1b1b" },
    { 0x774be0, 0x115, "30bc7d4d922f733905cfdb61a5c5eba8bdbb8add3f2bcd5c0dbd0b6759e2aaa7" }
};
constexpr unsigned MaxListEvictionLogs = 8;
constexpr UINT64 MaxCaptureTextureBytes = 256ull * 1024 * 1024;
constexpr unsigned MaxNgxEndpoints = 8;
constexpr UINT64 MaxEndpointResourceBytes = 256ull * 1024 * 1024;
constexpr unsigned MaxCaptureCandidates = 2, MaxObservedSubmissions = 256, MaxListsPerSubmission = 512;
constexpr ULONGLONG SubmissionWindowMs = 10000;
// The authored guide CS reads these five registers, all also read by High fog.
// One compile-time fixed register per draw avoids indexing by SV_Position, which
// can identify a coarse pixel outside the selected column when VRS is active.
// UINT loads/output preserve every bit, including special float bits.
constexpr std::array<UINT, 5> BoundCb12Registers { 21, 22, 23, 24, 27 };
constexpr char BoundCb12Shader[] = R"hlsl(
cbuffer SharedPixelConsts : register(b12) { uint4 sharedWords[28]; };
uint4 PSMain() : SV_Target0
{
    return sharedWords[BOUND_CB12_REGISTER];
}
)hlsl";

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

struct BoundCb12Variant
{
    ComPtr<ID3D12PipelineState> pso;
    std::string source, sourceSha256, bytecodeSha256;
};
struct TaggedPso
{
    ComPtr<ID3D12PipelineState> pso;
    const char* shader;
    ComPtr<ID3D12PipelineState> authored;
    std::array<BoundCb12Variant, BoundCb12Registers.size()> boundCb12;
    ComPtr<ID3D12RootSignature> root;
    std::vector<BYTE> vertexBytes, pixelBytes;
    D3D12_RENDER_TARGET_BLEND_DESC blend {};
    std::string pixelSha256;
    std::string cb12TemplateSha256;
};
struct RtvSlot
{
    // The descriptor itself does not own the resource. This pointer is borrowed
    // ONLY at its original valid OM bind, then AddRef'd before any GPU/async work.
    ID3D12Resource* resource = nullptr;
    D3D12_RENDER_TARGET_VIEW_DESC view {};
    uint64_t generation = 0;
    bool known = false;
    bool documentedDefault = false;
};
struct RtvHeap
{
    ComPtr<ID3D12DescriptorHeap> heap; // Keeps the CPU handle range from recycling.
    SIZE_T start = 0;
    UINT increment = 0;
    uint64_t generation = 0;
    UINT descriptorCount = 0;
    // Reserved descriptor address space is not populated metadata. Keep only
    // slots whose creation/copy we actually observed, with one global entry cap.
    std::unordered_map<UINT, RtvSlot> slots;
};
struct BoundRtv
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_RENDER_TARGET_VIEW_DESC view {};
    uint64_t heapGeneration = 0, slotGeneration = 0;
    UINT index = 0;
    bool known = false;
    bool documentedDefault = false;
};
struct CpuSrvHeap
{
    ComPtr<ID3D12DescriptorHeap> heap;
    SIZE_T start = 0;
    UINT increment = 0, count = 0;
    uint64_t generation = 0;
};
struct EarlyProducer
{
    Json metadata;
    std::array<FSRD::CyberpunkEngineAccess::TextureBorrow, 4> textures {};
    std::array<SIZE_T, 4> clearDescriptors {};
    std::array<ComPtr<ID3D12Resource>, 4> resources;
    ComPtr<IUnknown> list;
    uintptr_t nativeList = 0;
    uint64_t generation = 0;
    unsigned clearMask = 0, clearCount = 0;
    UINT64 resourceBytes = 0;
    std::string failure;
    bool valid = true;
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
struct CandidateState
{
    ComPtr<IUnknown> list;
    uint64_t generation = 0;
    Json metadata;
    bool reported = false, started = false;
};
struct SubmissionData
{
    ComPtr<IUnknown> queue;
    uint64_t entry = 0, exit = 0;
    Json lists;
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
    std::string sessionKey, sidecarRelative, fogCaptureId;
    std::vector<CandidateState> candidates;
    std::vector<std::shared_ptr<SubmissionData>> submissions;
    ULONGLONG startedAt = 0;
    unsigned observedSubmissions = 0;
    bool endpointsClosed = false, fogRecorded = false, finalized = false;
    std::string failure;
};
struct Registry
{
    std::mutex mutex;
    CryptoApi crypto;
    std::vector<TaggedPso> tagged;
    std::vector<RtvHeap> heaps;
    std::vector<CpuSrvHeap> cpuSrvHeaps;
    UINT64 cpuSrvSlots = 0, cpuSrvBytes = 0;
    std::vector<std::byte> guideShader;
    std::shared_ptr<EarlyProducer> earlyProducer;
    std::unordered_map<IUnknown*, ListState> lists;
    std::shared_ptr<EndpointTrace> endpoint;
    std::shared_ptr<EndpointTrace> submissionTrace;
    size_t slots = 0;
    uint64_t reservedRtvSlots = 0;
    uint64_t nextHeap = 0, nextSlot = 0, nextRecording = 0;
    size_t peakListCount = 0;
    uint64_t listEvictions = 0;
    unsigned listEvictionLogs = 0;
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
std::atomic<uintptr_t> authenticatedImage { 0 };
std::atomic<bool> earlyRequested { false }, earlyAttempted { false }, earlyHeapTrackingValid { true };
std::atomic<bool> earlyFatalRecording { false };
std::atomic<ULONGLONG> earlyRequestedAt { 0 };
std::atomic<bool> lightingRequested { false }, lightingAttempted { false };
std::atomic<ULONGLONG> lightingRequestedAt { 0 };
std::atomic<bool> endpointActive { false };
std::atomic<bool> submissionActive { false };
std::atomic<uint64_t> submissionSerial { 0 };
std::atomic<unsigned> drawLogs { 0 }, emptyLogs { 0 };
std::atomic<unsigned> arm { 0 };
std::atomic<uint64_t> scopes { 0 };
std::mutex hookMutex;

using FogNode = void(__fastcall*)(void* node, void* context);
FogNode originalFogNode = nullptr;
FogNode originalLightingNode = nullptr;
using FullscreenHelper = void(__fastcall*)(void*, uint32_t, uint8_t);
FullscreenHelper originalFullscreenHelper = nullptr;
using GBufferInitializer = void(__fastcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
GBufferInitializer originalGBufferInitializer = nullptr;
using SetPso = rewrite_signature<decltype(&ID3D12GraphicsCommandList::SetPipelineState)>::type;
using SetRtv = rewrite_signature<decltype(&ID3D12GraphicsCommandList::OMSetRenderTargets)>::type;
using Draw = rewrite_signature<decltype(&ID3D12GraphicsCommandList::DrawInstanced)>::type;
using DrawIndexed = rewrite_signature<decltype(&ID3D12GraphicsCommandList::DrawIndexedInstanced)>::type;
using CreateGraphics = rewrite_signature<decltype(&ID3D12Device::CreateGraphicsPipelineState)>::type;
using CreateCompute = rewrite_signature<decltype(&ID3D12Device::CreateComputePipelineState)>::type;
using CreateStream = rewrite_signature<decltype(&ID3D12Device2::CreatePipelineState)>::type;
SetPso originalSetPso = nullptr;
SetRtv originalSetRtv = nullptr;
Draw originalDraw = nullptr;
DrawIndexed originalDrawIndexed = nullptr;
CreateGraphics originalCreateGraphics = nullptr;
CreateCompute originalCreateCompute = nullptr;
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
using ClearRtv = rewrite_signature<decltype(&ID3D12GraphicsCommandList::ClearRenderTargetView)>::type;
using ClearDsv = rewrite_signature<decltype(&ID3D12GraphicsCommandList::ClearDepthStencilView)>::type;
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
ClearRtv originalClearRtv = nullptr;
ClearDsv originalClearDsv = nullptr;

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
thread_local EarlyProducer* producerScope = nullptr;

struct LightingScope
{
    LightingScope* previous;
    uint64_t serial;
    void* node;
    void* context;
    ID3D12GraphicsCommandList* psoList = nullptr;
    ComPtr<ID3D12PipelineState> pso;
    ID3D12GraphicsCommandList* rtvList = nullptr;
    UINT rtvCount = 0;
    BOOL contiguous = FALSE;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs {};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
    std::array<BoundRtv, 2> targets;
    bool finalHelper = false, hasDsv = false;
    unsigned finalDraws = 0;
};
thread_local LightingScope* lightingScope = nullptr;

bool ReadExactMemory(uintptr_t address, void* destination, size_t bytes) noexcept
{
    if (!address || !bytes || bytes > 65536 || bytes - 1 > UINTPTR_MAX - address)
        return false;
    SIZE_T actual = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination,
                             bytes, &actual) && actual == bytes;
}
template <typename T> bool ReadEarly(uintptr_t address, T& result) noexcept
{
    return ReadExactMemory(address, &result, sizeof(result));
}
template <typename T> bool ReadEarlyAt(uintptr_t base, uintptr_t offset, T& result) noexcept
{
    return base && offset <= UINTPTR_MAX - base && ReadEarly(base + offset, result);
}
bool MatchLiveCode(uintptr_t image, const FSRD::CyberpunkEngineAccess::CodeRange& code) noexcept
{
    try
    {
        if (!image || image > UINTPTR_MAX - FSRD::CyberpunkEngineAccess::ImageBytes || !code.bytes ||
            code.bytes > 65536 || code.rva > FSRD::CyberpunkEngineAccess::ImageBytes - code.bytes)
            return false;
        const auto begin = image + code.rva, end = begin + code.bytes;
        for (auto cursor = begin; cursor < end;)
        {
            MEMORY_BASIC_INFORMATION memory {};
            if (!VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) ||
                memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
                memory.AllocationBase != reinterpret_cast<void*>(image) || (memory.Protect & PAGE_GUARD) ||
                !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                return false;
            const auto region = uintptr_t(memory.BaseAddress);
            if (memory.RegionSize > UINTPTR_MAX - region || region + memory.RegionSize <= cursor)
                return false;
            cursor = std::min(end, region + memory.RegionSize);
        }
        std::vector<BYTE> bytes(code.bytes);
        if (!ReadExactMemory(begin, bytes.data(), bytes.size())) return false;
        Hash hash(Data().crypto);
        hash.Add(bytes.data(), ULONG(bytes.size()));
        return hash.Finish() == code.sha256;
    }
    catch (...) { return false; }
}

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

RtvSlot* FindRtv(Registry& data, SIZE_T handle, RtvHeap** owner = nullptr, UINT* index = nullptr,
                 bool createWrittenSlot = false)
{
    for (auto& heap : data.heaps)
    {
        if (handle < heap.start || !heap.increment)
            continue;
        const auto delta = handle - heap.start;
        const auto slot = delta / heap.increment;
        if (delta % heap.increment || slot >= heap.descriptorCount)
            continue;
        if (owner) *owner = &heap;
        if (index) *index = UINT(slot);
        if (auto found = heap.slots.find(UINT(slot)); found != heap.slots.end())
            return &found->second;
        if (!createWrittenSlot)
            return nullptr; // Unobserved source/binding must stay unknown, not allocate.
        if (data.slots >= MaxRtvSlots)
            throw std::runtime_error(std::format(
                "RTV provenance budget exhausted: reason=written slot limit retained_heaps={}/{} retained_slots={}/{} reserved_slots={} requested_heaps=0 requested_slots=1 heap_range_slots={}",
                data.heaps.size(), MaxRtvHeaps, data.slots, MaxRtvSlots, data.reservedRtvSlots, heap.descriptorCount));
        auto [written, inserted] = heap.slots.try_emplace(UINT(slot));
        if (inserted) ++data.slots;
        return &written->second;
    }
    return nullptr;
}

HRESULT WINAPI HookCreateHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                               REFIID iid, void** result)
{
    const HRESULT hr = originalCreateHeap(device, desc, iid, result);
    if (SUCCEEDED(hr) && desc && result && *result && captureEnabled.load() && !inMetadata &&
        earlyHeapTrackingValid.load() && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        desc->Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
        Metadata([&] {
            try
            {
                ComPtr<ID3D12DescriptorHeap> heap;
                if (FAILED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&heap))))
                    throw std::runtime_error("CPU SRV heap identity unavailable");
                const UINT increment = device->GetDescriptorHandleIncrementSize(desc->Type);
                const auto start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
                const UINT64 bytes = UINT64(desc->NumDescriptors) * increment;
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                if (std::any_of(data.cpuSrvHeaps.begin(), data.cpuSrvHeaps.end(),
                                [&](const auto& known) { return known.heap.Get() == heap.Get(); }))
                    return;
                if (!start || !increment || !desc->NumDescriptors || bytes > UINTPTR_MAX - start ||
                    data.cpuSrvHeaps.size() >= MaxCpuSrvHeaps ||
                    desc->NumDescriptors > MaxCpuSrvSlots - data.cpuSrvSlots || bytes > MaxCpuSrvBytes - data.cpuSrvBytes)
                    throw std::runtime_error(std::format(
                        "CPU SRV range budget exhausted: heaps={}/{} slots={}/{} bytes={}/{} requested_slots={} requested_bytes={}",
                        data.cpuSrvHeaps.size(), MaxCpuSrvHeaps, data.cpuSrvSlots, MaxCpuSrvSlots,
                        data.cpuSrvBytes, MaxCpuSrvBytes, desc->NumDescriptors, bytes));
                data.cpuSrvHeaps.push_back({ heap, start, increment, desc->NumDescriptors, ++data.nextHeap });
                data.cpuSrvSlots += desc->NumDescriptors;
                data.cpuSrvBytes += bytes;
            }
            catch (const std::exception& error)
            {
                // Failure disables only the optional early-guide experiment.
                earlyHeapTrackingValid.store(false);
                LOG_WARN("[FSRRR private guides] CPU descriptor range tracking disabled: {}", error.what());
            }
            catch (...) { earlyHeapTrackingValid.store(false); }
        });
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
            if (!desc->NumDescriptors || data.heaps.size() >= MaxRtvHeaps)
                throw std::runtime_error(std::format(
                    "RTV provenance budget exhausted: reason={} retained_heaps={}/{} retained_slots={}/{} reserved_slots={} requested_heaps=1 requested_heap_slots={} requested_metadata_slots=0",
                    !desc->NumDescriptors ? "zero-sized heap" : "heap limit", data.heaps.size(), MaxRtvHeaps,
                    data.slots, MaxRtvSlots, data.reservedRtvSlots, desc->NumDescriptors));
            RtvHeap record;
            record.heap = heap;
            record.start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
            record.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            record.generation = ++data.nextHeap;
            record.descriptorCount = desc->NumDescriptors;
            data.heaps.push_back(std::move(record));
            data.reservedRtvSlots += desc->NumDescriptors;
            const auto heapCount = data.heaps.size();
            if (heapCount == 1 || heapCount == 64 || heapCount == 128 || heapCount == 256 || heapCount == 512)
                LOG_INFO("[FSRRR fog capture] RTV provenance retained_heaps={}/{} retained_slots={}/{} reserved_slots={} requested_heaps=1 requested_heap_slots={} requested_metadata_slots=0 accepted; slot_payload_bytes={} (excludes map overhead)",
                         heapCount, MaxRtvHeaps, data.slots, MaxRtvSlots, data.reservedRtvSlots,
                         desc->NumDescriptors, data.slots * sizeof(RtvSlot));
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
        if (auto* slot = FindRtv(data, destination.ptr, nullptr, nullptr, true))
        {
            *slot = {};
            slot->generation = ++data.nextSlot;
            if (resource && desc)
            {
                slot->resource = resource;
                slot->view = *desc;
                slot->known = true;
            }
            else if (resource)
            {
                // Documented pDesc=null: inherit typed format/dimension, first mip,
                // all slices. This narrow single-slice/single-plane case is exact.
                // https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createrendertargetview
                const auto resourceDesc = resource->GetDesc();
                if (resourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                    resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    resourceDesc.DepthOrArraySize == 1 && resourceDesc.SampleDesc.Count == 1)
                {
                    slot->resource = resource;
                    slot->view.Format = resourceDesc.Format;
                    slot->view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    slot->view.Texture2D.MipSlice = 0;
                    slot->view.Texture2D.PlaneSlice = 0;
                    slot->known = true;
                    slot->documentedDefault = true;
                }
                // Typeless, array, MSAA and other default views remain unknown.
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
            if (auto* slot = FindRtv(data, destinations[range].ptr + SIZE_T(i) * increment, nullptr, nullptr, true))
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

struct LocatedSubmission
{
    std::shared_ptr<SubmissionData> submission;
    UINT index = 0;
    unsigned occurrences = 0;
};
LocatedSubmission LocateSubmission(const EndpointTrace& trace, const std::string& role)
{
    LocatedSubmission result;
    for (const auto& submission : trace.submissions)
        for (const auto& list : submission->lists)
            for (const auto& foundRole : list["roles"])
                if (foundRole == role)
                {
                    result.submission = submission;
                    result.index = list["array_index"].get<UINT>();
                    ++result.occurrences;
                }
    return result;
}

Json SubmissionRelation(const LocatedSubmission& fog, const LocatedSubmission& rr)
{
    const char* relation = "submission_not_observed";
    if (fog.occurrences > 1 || rr.occurrences > 1)
        relation = "repeated_recording_submission_ambiguous";
    else if (fog.submission && rr.submission && fog.submission->exit && rr.submission->exit)
    {
        if (fog.submission == rr.submission)
            relation = fog.index < rr.index ? "same_batch_array_order_only" : fog.index == rr.index ?
                "same_submitted_command_list" : "RR_list_precedes_fog_in_same_batch";
        else if (fog.submission->queue.Get() != rr.submission->queue.Get())
            relation = "different_queues_synchronization_not_observed";
        else if (fog.submission->exit < rr.submission->entry)
            relation = "ordered_nonoverlapping_same_queue_calls";
        else if (rr.submission->exit < fog.submission->entry)
            relation = "RR_submission_precedes_fog";
        else
            relation = "overlapping_queue_calls_order_unknown";
    }
    return { { "relation", relation }, { "fog_submission_occurrences", fog.occurrences },
        { "RR_submission_occurrences", rr.occurrences },
        { "resource_dependencies", "not_tracked" }, { "same_frame", "not_established" },
        { "unchanged_contents", "requires_independent_native_pixel_comparison" } };
}

void FinalizeSubmissionTrace(const std::shared_ptr<EndpointTrace>& trace) noexcept
{
    if (!trace)
        return;
    try
    {
        Json document;
        std::string relative;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            if (trace->finalized)
                return;
            const auto fog = LocateSubmission(*trace, "fog");
            bool ready = trace->fogRecorded && fog.occurrences == 1 && fog.submission->exit &&
                !trace->candidates.empty() &&
                (trace->candidates.size() == MaxCaptureCandidates || trace->endpointsClosed);
            Json candidates = Json::array(), submissions = Json::array();
            for (size_t i = 0; i < trace->candidates.size(); ++i)
            {
                const auto& candidate = trace->candidates[i];
                const auto rr = LocateSubmission(*trace, std::format("candidate-{}", i + 1));
                ready &= candidate.reported && (!candidate.started ||
                    (rr.occurrences == 1 && rr.submission->exit));
                candidates.push_back({ { "candidate", candidate.metadata },
                    { "capture_result_reported", candidate.reported }, { "capture_started", candidate.started },
                    { "submission_evidence", SubmissionRelation(fog, rr) } });
            }
            if (GetTickCount64() - trace->startedAt > SubmissionWindowMs && !ready)
                trace->failure = "submission observation window expired";
            if (!ready && trace->failure.empty())
                return;
            trace->finalized = true;
            for (const auto& submission : trace->submissions)
                submissions.push_back({ { "queue_identity", std::format("{:x}", uintptr_t(submission->queue.Get())) },
                    { "call_entry_serial", submission->entry }, { "call_exit_serial", submission->exit },
                    { "lists", submission->lists } });
            document = { { "schema", "optiscaler.fsr_rr.fog_submission_candidates.v1" },
                { "complete", ready && trace->failure.empty() }, { "failure", trace->failure },
                { "session_key", trace->sessionKey }, { "process_id", GetCurrentProcessId() },
                { "fog_capture_id", trace->fogCaptureId }, { "fog_origin", trace->fog },
                { "candidates", candidates }, { "submissions", submissions },
                { "observed_queue_calls", trace->observedSubmissions },
                { "status", "candidate_provenance_only_not_validated_pairing" },
                { "interval_semantics", "CPU call entry/return brackets; no new queue ordering or fences" },
                { "same_batch_warning", "array order alone does not prove workload completion or unchanged color" } };
            relative = trace->sidecarRelative;
            submissionActive.store(false, std::memory_order_release);
            data.submissionTrace.reset();
        }
        const auto path = Util::ExePath().parent_path() / relative;
        auto temporary = path;
        temporary += ".tmp";
        std::filesystem::create_directories(path.parent_path());
        const auto contents = document.dump(2);
        if (contents.size() > 2 * 1024 * 1024)
            throw std::runtime_error("submission sidecar size limit exceeded");
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot create unique submission sidecar");
        DWORD written = 0;
        const bool saved = WriteFile(file, contents.data(), DWORD(contents.size()), &written, nullptr) &&
                           written == contents.size();
        CloseHandle(file);
        if (!saved || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot finalize submission sidecar");
        LOG_INFO("[FSRRR fog endpoint] candidate submission provenance saved {}; complete={} (not a validated frame pair)",
                 path.string(), document["complete"].get<bool>());
    }
    catch (const std::exception& error)
    {
        try { LOG_WARN("[FSRRR fog endpoint] submission sidecar incomplete: {}", error.what()); } catch (...) {}
    }
    catch (...) {}
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
            uint64_t evictedGeneration = 0;
            uintptr_t evictedIdentity = 0;
            if (!data.lists.contains(identity.Get()) && data.lists.size() >= MaxCommandLists)
            {
                // This registry is evidence, not object ownership. Pool churn must
                // not disable all diagnostics. An evicted list stays UNKNOWN in
                // TrackList/PrepareCapture/ObserveNgxInput until a new observed Reset;
                // no state is reconstructed from pointers or later draw calls.
                const auto oldest = std::min_element(data.lists.begin(), data.lists.end(),
                    [](const auto& a, const auto& b) { return a.second.generation < b.second.generation; });
                evictedGeneration = oldest->second.generation;
                evictedIdentity = uintptr_t(oldest->first);
                data.lists.erase(oldest);
                ++data.listEvictions;
            }
            auto& state = data.lists[identity.Get()];
            state = {};
            state.generation = ++data.nextRecording;
            state.known = true;
            if (evictedGeneration && data.listEvictionLogs < MaxListEvictionLogs)
            {
                ++data.listEvictionLogs;
                LOG_INFO("[FSRRR fog capture] command-list provenance retained={}/{} total_evictions={} evicted_identity={:x} evicted_Reset_generation={} incoming_identity={:x} incoming_Reset_generation={}; evicted lists remain unknown until Reset; eviction_log={}/{}",
                         data.lists.size(), MaxCommandLists, data.listEvictions, evictedIdentity, evictedGeneration,
                         uintptr_t(identity.Get()), state.generation, data.listEvictionLogs, MaxListEvictionLogs);
            }
            if (data.lists.size() > data.peakListCount)
            {
                data.peakListCount = data.lists.size();
                const auto count = data.peakListCount;
                if (count == 1 || count == 128 || count == 512 || count == 1024 || count == MaxCommandLists)
                    LOG_INFO("[FSRRR fog capture] command-list provenance retained={}/{} peak={} total_evictions={} state_payload_bytes={} (excludes map overhead)",
                             data.lists.size(), MaxCommandLists, count, data.listEvictions,
                             data.lists.size() * sizeof(ListState));
            }
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
            if (lightingRequested.load() && !lightingAttempted.load() &&
                GetTickCount64() - lightingRequestedAt.load() >= 10000)
            {
                lightingAttempted.store(true);
                FSRDFogLayerCapture::CancelEarlyGuideRequest();
                LOG_WARN("[FSRRR lighting guides] request timed out without the exact final lighting draw; no private dispatch");
            }
            const auto lightingRequest = Util::ExePath().parent_path() / L"FSRRR-lighting-guides.request";
            const auto lightingAttributes = GetFileAttributesW(lightingRequest.c_str());
            if (!lightingAttempted.load() && lightingAttributes != INVALID_FILE_ATTRIBUTES &&
                !(lightingAttributes & FILE_ATTRIBUTE_DIRECTORY) && FSRDFogLayerCapture::RequestEarlyGuides())
            {
                if (DeleteFileW(lightingRequest.c_str()))
                {
                    lightingRequestedAt.store(GetTickCount64());
                    lightingRequested.store(true);
                    LOG_INFO("[FSRRR lighting guides] one-shot armed; private outputs only after exact original final lighting draw");
                }
                else FSRDFogLayerCapture::CancelEarlyGuideRequest();
            }
            const auto captureRequest = Util::ExePath().parent_path() / L"FSRRR-fog-capture.request";
            const auto captureAttributes = GetFileAttributesW(captureRequest.c_str());
            if (captureAttributes != INVALID_FILE_ATTRIBUTES && !(captureAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                FSRDFogLayerCapture::Request())
            {
                if (DeleteFileW(captureRequest.c_str()))
                {
                    const auto earlyRequest = Util::ExePath().parent_path() / L"FSRRR-early-guides.request";
                    const auto earlyAttributes = GetFileAttributesW(earlyRequest.c_str());
                    if (!earlyAttempted.load() && earlyAttributes != INVALID_FILE_ATTRIBUTES &&
                        !(earlyAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW(earlyRequest.c_str()))
                    {
                        earlyRequestedAt.store(GetTickCount64());
                        earlyRequested.store(true);
                        LOG_INFO("[FSRRR private guides] one-shot armed; waiting up to 10s for original current GBuffer clears on the Fog recording");
                    }
                    LOG_INFO("[FSRRR fog capture] explicit one-shot request queued; waiting for fully authenticated draw/state");
                }
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

const Json& EarlyInput(const Json& metadata, size_t index)
{
    constexpr uint32_t keys[] = { 0x63bcf380, 0x64bcf513, 0x65bcf6a6, 0x61f178d4 };
    const auto& input = metadata.at("inputs").at(index);
    const auto& interval = input.at("logical_interval");
    const auto& registry = input.at("texture_registry");
    if (input.at("base_key").get<uint32_t>() != keys[index] || input.at("status") != "handle_present" ||
        interval.at("status") != "compiler_interval_observed" || !interval.at("repeated_metadata_equal").get<bool>() ||
        !interval.at("inclusive_contains_position").get<bool>() || interval.at("end_event_relation") != "before" ||
        interval.at("graph_phase").get<unsigned>() != 2 || interval.at("record_used_flag").get<unsigned>() != 1 ||
        interval.at("record_handle") != input.at("handle") || registry.at("status") != "borrowed_address_observed" ||
        registry.at("ref_status").get<int32_t>() <= 0 || registry.at("ref_status") != registry.at("ref_status_after"))
        throw std::runtime_error("current guide handle/reservation/registry evidence incomplete");
    const auto position = interval.at("current_position").get<uint64_t>();
    if (interval.at("holder_first_use").get<uint64_t>() > position ||
        position >= interval.at("holder_end_event_position").get<uint64_t>() ||
        interval.at("holder_end_event_position").get<uint64_t>() > interval.at("holder_reservation_end").get<uint64_t>())
        throw std::runtime_error("current guide is outside its pre-end-event logical reservation");
    return input;
}

uint32_t EarlyFrameSource(const Json& metadata)
{
    const auto& source = metadata.at("camera_provenance").at("frame_id_virtual_route").at("explicit_frame_id_source");
    if (source.at("status") != "CPU_value_present" || !source.at("repeated_source_fields_equal").get<bool>())
        throw std::runtime_error("current explicit CPU frame-ID source unavailable");
    return source.at("source_value").get<uint32_t>();
}

void __fastcall HookGBufferInitializer(void* context, uint32_t h0, uint32_t h1, uint32_t h2, uint32_t hs)
{
    PollRearm();
    std::shared_ptr<EarlyProducer> candidate;
    if (earlyRequested.load() && !earlyAttempted.load() && !inMetadata)
        Metadata([&] {
            try
            {
                auto value = std::make_shared<EarlyProducer>();
                candidate = value;
                value->metadata = Json::parse(FSRDCyberpunkEarlyGuides::Describe(context, authenticatedImage.load()));
                EarlyFrameSource(value->metadata);
                const uint32_t handles[] = { h0, h1, h2, hs };
                uintptr_t registry = 0;
                if (!ReadEarly(authenticatedImage.load() + FSRD::CyberpunkEngineAccess::RegistryRva, registry) || !registry)
                    throw std::runtime_error("initializer registry unavailable");
                for (size_t i = 0; i < 4; ++i)
                {
                    const auto& input = EarlyInput(value->metadata, i);
                    if (input.at("handle").get<uint32_t>() != handles[i] || !handles[i] || handles[i] > 0x8000)
                        throw std::runtime_error("initializer arguments differ from selected graph versions");
                    value->textures[i] = { handles[i], input.at("texture_registry").at("borrowed_native_address").get<uintptr_t>() };
                    const auto slot = registry + 0x2f1d8 + uintptr_t(handles[i] - 1) * 0xb0;
                    if (i < 3)
                    {
                        uintptr_t perMipViews = 0;
                        if (!ReadEarly(slot + 0x40, perMipViews) || perMipViews)
                            throw std::runtime_error("initializer per-mip RTV route unsupported");
                    }
                    if (!ReadEarly(slot + (i < 3 ? 0x18 : 0x20), value->clearDescriptors[i]) || !value->clearDescriptors[i])
                        throw std::runtime_error("initializer exact clear descriptor unavailable");
                }
                candidate = std::move(value);
            }
            catch (const std::exception& error)
            {
                if (candidate) { candidate->valid = false; candidate->failure = error.what(); }
                if (captureRefusalLogs.fetch_add(1) < 8)
                    LOG_WARN("[FSRRR private guides] original initializer evidence unavailable: {}", error.what());
            }
        });
    auto* previous = producerScope;
    producerScope = candidate.get();
    struct RestoreProducer { EarlyProducer* previous; ~RestoreProducer() { producerScope = previous; } } restore { previous };
    originalGBufferInitializer(context, h0, h1, h2, hs);
    if (candidate)
        Metadata([&] {
            std::shared_ptr<EarlyProducer> retired;
            auto& data = Data();
            {
                std::lock_guard lock(data.mutex);
                retired = std::move(data.earlyProducer);
                data.earlyProducer = candidate; // At most one bounded, owned original-clear observation.
            } // Final COM releases of the old producer occur outside the registry lock.
        });
}

// This is a valid native API use boundary, not an AddRef on a sampled last pointer.
// The exact initializer arguments, registry descriptor route and live original
// clear identify the resources owned by the executing graph for this operation.
void ObserveInitializerClear(ID3D12GraphicsCommandList* list, SIZE_T descriptor, bool depthStencil,
                             UINT rectangles, const D3D12_RECT* rects, D3D12_CLEAR_FLAGS flags)
{
    if (!producerScope || inMetadata) return;
    Metadata([&] {
        auto& producer = *producerScope;
        try
        {
            if (!producer.valid || rectangles || rects ||
                (depthStencil && flags != (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)))
                throw std::runtime_error("initializer clear not an exact full-resource clear");
            const auto index = producer.clearCount;
            if (index >= 4 || depthStencil != (index == 3) || descriptor != producer.clearDescriptors[index])
                throw std::runtime_error("initializer clear order/descriptor differs");
            const auto identity = ListIdentity(list);
            uint64_t generation = 0;
            {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                const auto found = data.lists.find(identity.Get());
                if (found == data.lists.end() || !found->second.known || found->second.predicated ||
                    found->second.renderPass || found->second.queryCount)
                    throw std::runtime_error("initializer list Reset/predication/query state unknown");
                generation = found->second.generation;
                if (index < 3)
                {
                    const auto* rtv = FindRtv(data, descriptor);
                    if (!rtv || !rtv->known || uintptr_t(rtv->resource) != producer.textures[index].native ||
                        rtv->view.ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D || rtv->view.Texture2D.MipSlice ||
                        rtv->view.Texture2D.PlaneSlice)
                        throw std::runtime_error("initializer RTV creation provenance unavailable");
                }
            }
            if (!index)
            {
                producer.list = identity;
                producer.nativeList = uintptr_t(list);
                producer.generation = generation;
            }
            else if (producer.list.Get() != identity.Get() || producer.nativeList != uintptr_t(list) ||
                     producer.generation != generation)
                throw std::runtime_error("initializer clears span different list recordings");
            uintptr_t registry = 0, native = 0, clearDescriptor = 0;
            int32_t refs = 0;
            if (!ReadEarly(authenticatedImage.load() + FSRD::CyberpunkEngineAccess::RegistryRva, registry) || !registry)
                throw std::runtime_error("initializer registry disappeared at original clear");
            const auto slot = registry + 0x2f1d8 + uintptr_t(producer.textures[index].handle - 1) * 0xb0;
            if (!ReadEarly(slot - 8, refs) || refs <= 0 || !ReadEarly(slot, native) || native != producer.textures[index].native ||
                !ReadEarly(slot + (index < 3 ? 0x18 : 0x20), clearDescriptor) || clearDescriptor != descriptor)
                throw std::runtime_error("initializer resource/descriptor changed at original clear");
            ComPtr<ID3D12Resource> resource = reinterpret_cast<ID3D12Resource*>(native);
            const auto desc = resource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
                desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.Width > 8192 || desc.Height > 8192)
                throw std::runtime_error("initializer clear subresource layout unsupported");
            ComPtr<ID3D12Device> device;
            if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
                throw std::runtime_error("initializer resource device unavailable");
            const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            constexpr UINT64 MaxInitializerBytes = 128ull * 1024 * 1024;
            if (!bytes || bytes > MaxInitializerBytes - producer.resourceBytes)
                throw std::runtime_error("initializer resource ownership exceeds 128MiB");
            producer.resourceBytes += bytes;
            producer.resources[index] = std::move(resource);
            ++producer.clearCount;
            producer.clearMask |= 1u << index;
        }
        catch (const std::exception& error) { producer.valid = false; producer.failure = error.what(); }
        catch (...) { producer.valid = false; }
    });
}

void WINAPI HookClearRtv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
                         const FLOAT color[4], UINT rectangles, const D3D12_RECT* rects)
{
    ObserveInitializerClear(list, descriptor.ptr, false, rectangles, rects, D3D12_CLEAR_FLAGS(0));
    originalClearRtv(list, descriptor, color, rectangles, rects);
}
void WINAPI HookClearDsv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
                         D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rectangles, const D3D12_RECT* rects)
{
    if (producerScope && stencil != 0) producerScope->valid = false;
    ObserveInitializerClear(list, descriptor.ptr, true, rectangles, rects, flags);
    originalClearDsv(list, descriptor, flags, depth, stencil, rectangles, rects);
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

void __fastcall HookLightingNode(void* node, void* context)
{
    PollRearm();
    LightingScope current { lightingScope, scopes.fetch_add(1) + 1, node, context };
    lightingScope = &current;
    struct Restore { LightingScope* previous; ~Restore() { lightingScope = previous; } } restore { current.previous };
    originalLightingNode(node, context); // Exactly one original callback, including ordinary refusal paths.
}

void __fastcall HookFullscreenHelper(void* renderer, uint32_t shader, uint8_t flag)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* current = lightingScope;
    const bool previous = current && current->finalHelper;
    if (current)
        current->finalHelper = !inMetadata && caller == authenticatedImage.load() + FinalLightingHelperReturnRva;
    struct Restore { LightingScope* current; bool previous; ~Restore() { if (current) current->finalHelper = previous; } }
        restore { current, previous };
    originalFullscreenHelper(renderer, shader, flag); // Scope only; never invoke an extra engine draw.
}

void WINAPI HookSetPso(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    if (lightingScope && !inMetadata && lightingRequested.load() && !lightingAttempted.load())
        Metadata([&] {
            lightingScope->psoList = list;
            lightingScope->pso = pso; // Original valid SetPipelineState borrow boundary.
        });
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
    if (lightingScope && !inMetadata && lightingRequested.load() && !lightingAttempted.load())
        Metadata([&] {
            auto& current = *lightingScope;
            current.rtvList = list;
            current.rtvCount = count;
            current.contiguous = contiguous;
            current.hasDsv = dsv != nullptr;
            current.dsv = dsv ? *dsv : D3D12_CPU_DESCRIPTOR_HANDLE {};
            current.rtvs = {};
            current.targets = {};
            if (count != 2 || !rtvs) return;
            ComPtr<ID3D12Device> device;
            if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return;
            const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            if (!increment || rtvs[0].ptr > SIZE_MAX - increment) return;
            for (UINT i = 0; i < 2; ++i)
            {
                BoundRtv bound;
                ID3D12Resource* borrowed = nullptr;
                {
                    auto& data = Data();
                    std::lock_guard lock(data.mutex);
                    RtvHeap* heap = nullptr;
                    UINT index = 0;
                    const auto* first = FindRtv(data, rtvs[0].ptr, &heap, &index);
                    if (!first || !heap) return;
                    const SIZE_T handle = contiguous ? rtvs[0].ptr + SIZE_T(i) * increment : rtvs[i].ptr;
                    const auto* slot = FindRtv(data, handle, &heap, &index);
                    if (!slot || !slot->known || !slot->resource) return;
                    borrowed = slot->resource;
                    bound.heap = heap->heap;
                    bound.view = slot->view;
                    bound.heapGeneration = heap->generation;
                    bound.slotGeneration = slot->generation;
                    bound.index = index;
                    bound.known = true;
                    bound.documentedDefault = slot->documentedDefault;
                    current.rtvs[i].ptr = handle;
                }
                // Original OM bind is a valid borrow; no sampled resource AddRef
                // and no final COM release while the metadata registry is locked.
                bound.resource = borrowed;
                current.targets[i] = std::move(bound);
            }
        });
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
                bound.documentedDefault = slot->documentedDefault;
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
    std::array<ComPtr<ID3D12PipelineState>, BoundCb12Registers.size()> boundCb12Psos;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> sourceHeap, privateHeap, boundCb12Heap;
    D3D12_CPU_DESCRIPTOR_HANDLE originalRtv {}, frozenOriginalRtv {}, authoredRtv {}, boundCb12Rtv {};
    D3D12_RENDER_TARGET_VIEW_DESC originalView {};
    BOOL contiguous = FALSE;
    FSRDFogLayerCapture::Layers layers;
    Json provenance;
    std::shared_ptr<EndpointTrace> endpoint;
    ListState drawState;
    std::shared_ptr<FSRD::CyberpunkGuidePass::Work> earlyWork;
    bool fatalEarlyRecording = false;
};

bool SameEarlyReservation(const Json& earlier, const Json& current, size_t index)
{
    const auto& a = EarlyInput(earlier, index);
    const auto& b = EarlyInput(current, index);
    for (const auto* field : { "base_key", "namespaced_key", "handle" })
        if (a.at(field) != b.at(field)) return false;
    for (const auto* field : { "graph_address", "holder_address", "resource_record_address", "holder_first_use",
                              "holder_reservation_end", "holder_end_event_position", "record_handle", "record_kind", "record_policy" })
        if (a.at("logical_interval").at(field) != b.at("logical_interval").at(field)) return false;
    if (a.at("logical_interval").at("current_position").get<uint64_t>() >
        b.at("logical_interval").at("current_position").get<uint64_t>()) return false;
    return a.at("texture_registry").at("borrowed_native_address") == b.at("texture_registry").at("borrowed_native_address");
}

bool ProducerMatches(const EarlyProducer& producer, const Json& current, ID3D12GraphicsCommandList* list,
                     uint64_t generation) noexcept
{
    try
    {
        if (!producer.valid || producer.clearMask != 15 || producer.clearCount != 4 ||
            producer.nativeList != uintptr_t(list) || producer.generation != generation ||
            producer.metadata.at("view") != current.at("view") ||
            producer.metadata.at("view_dimensions") != current.at("view_dimensions") ||
            EarlyFrameSource(producer.metadata) != EarlyFrameSource(current))
            return false;
        for (size_t i = 0; i < 4; ++i)
            if (!producer.resources[i] || !SameEarlyReservation(producer.metadata, current, i)) return false;
        return true;
    }
    catch (...) { return false; }
}

struct EarlyEngineHost
{
    const CapturePlan& plan;
    const EarlyProducer& producer;
    uintptr_t context = 0, view = 0;
    uint64_t serial = 0;

    bool Read(uintptr_t address, void* destination, size_t bytes) noexcept
    { return ReadExactMemory(address, destination, bytes); }
    uint32_t ThreadId() noexcept { return GetCurrentThreadId(); }
    bool ReadTlsSlotZero(uintptr_t& result) noexcept
    {
        const auto slots = uintptr_t(__readgsqword(0x58));
        return ReadEarly(slots, result) && result;
    }
    bool ExactImageAuthenticated(uintptr_t image, uintptr_t size, uint32_t stamp, std::string_view sha) noexcept
    {
        return active.load() && captureEnabled.load() && earlyRequested.load() &&
            image == authenticatedImage.load() && image == uintptr_t(GetModuleHandleW(nullptr)) &&
            size == 0x04efc000 && stamp == 0x68af45ea && sha == ExeSha256;
    }
    bool LiveCodeMatches(uintptr_t image, const FSRD::CyberpunkEngineAccess::CodeRange& code) noexcept
    { return MatchLiveCode(image, code); }
    bool IsAdmittedFogScope(uint64_t requested, uintptr_t list, uintptr_t pso,
                            const std::array<FSRD::CyberpunkEngineAccess::TextureBorrow, 4>& inputs) noexcept
    {
        // No game calls occur inside privateWork, so the synchronous original Fog
        // graph reservation stays active. Do not resample a global frame counter
        // here: it may advance independently of this already-admitted recording.
        if (!scope || scope->serial != serial || requested != serial || uintptr_t(scope->context) != context ||
            scope->psoList != reinterpret_cast<ID3D12GraphicsCommandList*>(list) || uintptr_t(scope->pso) != pso ||
            pso != uintptr_t(plan.originalPso.Get()) || producer.nativeList != list ||
            producer.generation != plan.drawState.generation || !producer.valid || producer.clearMask != 15 ||
            scope->hasDsv || scope->rtvCount != 1 || !captureTrackingValid.load())
            return false;
        uintptr_t currentView = 0;
        uint8_t flags = 0;
        if (!ReadEarly(context + 0x18, currentView) || currentView != view ||
            !ReadEarly(context + 0x30, flags) || !(flags & 2)) return false;
        for (size_t i = 0; i < 4; ++i)
            if (inputs[i].handle != producer.textures[i].handle || inputs[i].native != producer.textures[i].native ||
                inputs[i].native == uintptr_t(plan.main.Get()) || !producer.resources[i]) return false;
        return true;
    }
    bool ListIsDirect(uintptr_t list) noexcept
    { return reinterpret_cast<ID3D12GraphicsCommandList*>(list)->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT; }
    uintptr_t CurrentNativeList(uintptr_t address) noexcept
    { return uintptr_t(reinterpret_cast<void*(__fastcall*)()>(address)()); }
    void RequestState(uintptr_t address, uintptr_t engine, uint32_t handle, uint32_t state, uint32_t subresource) noexcept
    { reinterpret_cast<void(__fastcall*)(void*, uint32_t, uint32_t, uint32_t)>(address)(reinterpret_cast<void*>(engine), handle, state, subresource); }
    void Flush(uintptr_t address, uintptr_t engine) noexcept
    { reinterpret_cast<void(__fastcall*)(void*)>(address)(reinterpret_cast<void*>(engine)); }
    void Reenter(uintptr_t address, uintptr_t list) noexcept
    { reinterpret_cast<void(__fastcall*)(ID3D12GraphicsCommandList*)>(address)(reinterpret_cast<ID3D12GraphicsCommandList*>(list)); }
    void RestorePso(uintptr_t list, uintptr_t pso) noexcept
    { originalSetPso(reinterpret_cast<ID3D12GraphicsCommandList*>(list), reinterpret_cast<ID3D12PipelineState*>(pso)); }
};

void PrepareAndRecordEarlyGuides(ID3D12GraphicsCommandList* list, CapturePlan& plan)
{
    if (!earlyRequested.load() || earlyAttempted.exchange(true)) return;
    auto& evidence = plan.provenance["early_guides"];
    evidence = { { "schema", "optiscaler.fsr_rr.private_early_guides.v1" }, { "status", "refused" },
        { "original_scene_modified", false }, { "initializer_rva", GBufferInitializerRva },
        { "transparent_input", "explicitly_pre_transparency_t3_t6_disabled" }, { "last_material_writer", "not_proven" },
        { "GPU_completion", "requires_companion_completion_fence" }, { "SL_frame_association", "not_asserted" } };
    try
    {
        if (!originalGBufferInitializer || !originalClearRtv || !originalClearDsv || !originalSetPso ||
            !earlyHeapTrackingValid.load()) throw std::runtime_error("early producer/state/CPU descriptor tracking unavailable");
        std::shared_ptr<EarlyProducer> producer;
        std::vector<std::byte> shader;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            producer = data.earlyProducer;
            shader = data.guideShader;
        }
        const auto& current = plan.provenance.at("early_guide_availability");
        auto frameEvidence = [](const Json& metadata) -> Json {
            try { return EarlyFrameSource(metadata); } catch (...) { return nullptr; }
        };
        evidence["fog_observation"] = { { "native_list", uintptr_t(list) }, { "recording_generation", plan.drawState.generation },
            { "view", current.value("view", uintptr_t(0)) }, { "cpu_frame_source", frameEvidence(current) },
            { "position", current.value("graph_position", uint32_t(0)) } };
        if (producer)
        {
            evidence["initializer_observation"] = { { "native_list", producer->nativeList },
                { "recording_generation", producer->generation }, { "clear_mask", producer->clearMask },
                { "clear_count", producer->clearCount }, { "valid", producer->valid }, { "failure", producer->failure },
                { "view", producer->metadata.value("view", uintptr_t(0)) },
                { "cpu_frame_source", frameEvidence(producer->metadata) },
                { "position", producer->metadata.value("graph_position", uint32_t(0)) },
                { "inputs", Json::array() } };
            if (producer->metadata.contains("inputs") && producer->metadata["inputs"].is_array())
                for (size_t i = 0; i < std::min(size_t(4), producer->metadata["inputs"].size()); ++i)
                    evidence["initializer_observation"]["inputs"].push_back(producer->metadata["inputs"][i]);
        }
        else evidence["initializer_observation"] = nullptr;
        if (!producer || !ProducerMatches(*producer, current, list, plan.drawState.generation))
            throw std::runtime_error("no matching four original full clears on same current view/frame-source/list Reset and reservations");
        const auto image = authenticatedImage.load();
        const auto view = current.at("view").get<uintptr_t>();
        const auto dimensions = current.at("view_dimensions").get<std::array<uint32_t, 2>>();
        const auto mainDesc = plan.main->GetDesc();
        if (dimensions[0] != mainDesc.Width || dimensions[1] != mainDesc.Height || shader.size() != GuideShaderBytes)
            throw std::runtime_error("current guide/scene extent or runtime authenticated guide shader unavailable");
        FSRD::CyberpunkGuideMatrix::MatrixWords inverseProjection {}, inverseView {}, checkProjection {}, checkView {};
        if (!ReadEarly(view + 0x1c0, inverseProjection) || !ReadEarly(view + 0x180, inverseView) ||
            !ReadEarly(view + 0x1c0, checkProjection) || !ReadEarly(view + 0x180, checkView) ||
            inverseProjection != checkProjection || inverseView != checkView)
            throw std::runtime_error("current inverse native camera matrix reads unavailable/changed");
        const auto& matrices = current.at("camera_provenance").at("matrices");
        for (const auto& item : { std::pair { "inverse_native_projection_jittered", &inverseProjection },
                                  std::pair { "inverse_native_view", &inverseView } })
        {
            const auto rows = matrices.at(item.first).at("source_uint32_rows").get<std::array<std::array<uint32_t, 4>, 4>>();
            if (std::memcmp(rows.data(), item.second->data(), 64) != 0)
                throw std::runtime_error("camera source changed since same-scope metadata");
        }
        FSRD::CyberpunkGuideConstants::ObservedSharedWords sharedWords {};
        if (!FSRD::CyberpunkGuideMatrix::Generate(inverseProjection, inverseView, dimensions[0], dimensions[1], sharedWords))
            throw std::runtime_error("exact authored CPU matrix arithmetic contract unavailable");
        const auto& settings = current.at("guide_settings");
        if (settings.at("extra_specular_enabled").get<unsigned>() != 0)
            throw std::runtime_error("authored extra-specular branch enabled; its unavailable t5 cannot be disabled implicitly");
        FSRD::CyberpunkGuideConstants::PassSources passSources;
        passSources.width = dimensions[0]; passSources.height = dimensions[1];
        passSources.transparency = FSRD::CyberpunkGuideConstants::TransparencyInput::PreTransparencySurface;
        passSources.noVMode = settings.at("NoV_mode").get<int32_t>();
        passSources.extraSpecularScaleBits = settings.value("extra_specular_scale_bits", uint32_t(0));
        FSRD::CyberpunkGuideConstants::PassConstants pass {};
        if (!FSRD::CyberpunkGuideConstants::PackPass(passSources, pass))
            throw std::runtime_error("authored private guide pass constants invalid");
        std::array<FSRD::CyberpunkGuidePass::SourceView, 4> sources;
        Json views = Json::array();
        for (size_t i = 0; i < 4; ++i)
        {
            const auto& source = EarlyInput(current, i);
            const auto& descriptors = source.at("texture_registry").at("descriptor_sources");
            if (descriptors.at("status") != "cpu_descriptor_sources_observed" ||
                !descriptors.at("repeated_source_fields_equal").get<bool>())
                throw std::runtime_error("exact authored CPU SRV source unavailable");
            if (i == 3)
            {
                const auto formatTag = descriptors.at("raw_format_sample_bits").get<uint32_t>() & 0x3f;
                const auto resourceFormat = producer->resources[i]->GetDesc().Format;
                // Exact alternate stencil-view creation branch in RVA2221f4:
                // tags0x18/0x19 -> X24_G8 / X32_G8X24, green stencil component,
                // PlaneSlice1, mip0/count1. Never reinterpret a generic UINT2 SRV.
                if (descriptors.at("raw_array_size").get<uint32_t>() != 1 ||
                    (descriptors.at("raw_dimension_mip_bits").get<uint32_t>() & 0xf) != 0 ||
                    (descriptors.at("raw_flags_bits").get<uint32_t>() & 5) != 5 ||
                    (formatTag != 0x18 && formatTag != 0x19) ||
                    resourceFormat != (formatTag == 0x18 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS))
                    throw std::runtime_error("t4 source does not satisfy authenticated alternate stencil-view factory branch");
            }
            const auto descriptor = descriptors.at(i == 3 ? "alternate_cpu_srv_handle" : "ordinary_cpu_srv_handle").get<SIZE_T>();
            if (!descriptor || producer->textures[i].native == uintptr_t(plan.main.Get()))
                throw std::runtime_error("guide source missing or aliases live scene writable target");
            sources[i].resource = producer->resources[i];
            sources[i].descriptor.ptr = descriptor;
            uint64_t heapGeneration = 0;
            {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                for (const auto& heap : data.cpuSrvHeaps)
                    if (descriptor >= heap.start && (descriptor - heap.start) % heap.increment == 0 &&
                        (descriptor - heap.start) / heap.increment < heap.count)
                    {
                        if (sources[i].heap) throw std::runtime_error("CPU SRV source range ambiguous");
                        sources[i].heap = heap.heap;
                        heapGeneration = heap.generation;
                    }
            }
            if (!sources[i].heap) throw std::runtime_error("authored CPU SRV source has no retained heap range");
            views.push_back({ { "binding", i == 3 ? 4 : i }, { "handle", producer->textures[i].handle },
                { "native", producer->textures[i].native }, { "cpu_srv_handle", descriptor }, { "heap_generation", heapGeneration },
                { "route", i == 3 ? "authored_alternate_stencil_srv" : "authored_ordinary_srv" },
                { "descriptor_transform", "none; CopyDescriptorsSimple from exact engine slot" } });
        }
        // Re-read the actual current graph reservation/settings after preparation,
        // BEFORE descriptor copies or engine state mutation. Native resources are
        // owned since the original clear; descriptor slots stay graph-owned here.
        const auto fresh = Json::parse(FSRDCyberpunkEarlyGuides::Describe(scope->context, image));
        if (!ProducerMatches(*producer, fresh, list, plan.drawState.generation) ||
            fresh.at("guide_settings") != current.at("guide_settings") ||
            fresh.at("camera_provenance").at("matrices") != matrices)
            throw std::runtime_error("source reservation/settings/camera changed before private preparation");
        for (size_t i = 0; i < 4; ++i)
            if (EarlyInput(fresh, i).at("texture_registry").at("descriptor_sources") !=
                EarlyInput(current, i).at("texture_registry").at("descriptor_sources"))
                throw std::runtime_error("authored source descriptor fields changed before private copy");
        const auto shared = FSRD::CyberpunkGuideConstants::PackShared(sharedWords);
        auto work = FSRD::CyberpunkGuidePass::Prepare(plan.device.Get(), dimensions[0], dimensions[1], sources, pass, shared, shader);
        if (!work) throw std::runtime_error("private guide PSO/resource preparation refused");
        evidence["sources"] = std::move(views);
        evidence["initializer_cpu_frame_source"] = EarlyFrameSource(producer->metadata);
        evidence["fog_cpu_frame_source"] = EarlyFrameSource(current);
        evidence["recording_generation"] = plan.drawState.generation;
        evidence["initialization_proof"] = "four original full clear calls precede private dispatch on same native list/Reset";
        evidence["cb12_words"] = sharedWords;
        evidence["cb12_recipe"] = "exact authored SSE-order inverse jittered native P times rotation-only inverse native V";
        evidence["cb6_words"] = pass;
        evidence["shader_sha256"] = GuideShaderSha256;
        EarlyEngineHost host { plan, *producer, uintptr_t(scope->context), view, scope->serial };
        FSRD::CyberpunkEngineAccess::Input input;
        input.image = image; input.list = uintptr_t(list); input.originalPso = uintptr_t(plan.originalPso.Get());
        input.originalFogScope = scope->serial; input.textures = producer->textures;
        const auto result = FSRD::CyberpunkEngineAccess::RecordPrivateCompute(host, input, [&] { return work->Record(list); });
        if (result.outcome == FSRD::CyberpunkEngineAccess::Outcome::ScopeLostAfterPrivate)
        {
            // Set a nonthrowing global latch BEFORE any allocation/logging. It
            // remains visible to HookDraw even if PrepareCapture cannot return its plan.
            earlyFatalRecording.store(true);
            plan.fatalEarlyRecording = true;
            try { LOG_ERROR("[FSRRR private guides] FATAL research recording lost original engine scope; refusing original draw and terminating authenticated Cyberpunk only"); }
            catch (...) {}
            if (active.load() && captureEnabled.load() && earlyRequested.load() && image == authenticatedImage.load() &&
                image == uintptr_t(GetModuleHandleW(nullptr)))
            {
                // User explicitly authorizes stopping the game. Never resume an
                // invalid recording, kill another PID, or terminate on ordinary refusal.
                TerminateProcess(GetCurrentProcess(), 0xf51d0001u);
                RaiseFailFastException(nullptr, nullptr, 0);
            }
            return; // Defensive: HookDraw never resumes a flagged invalid recording.
        }
        evidence["engine_state_requests"] = result.requestsIssued;
        evidence["callback_entered"] = result.callbackEntered;
        evidence["bindings_restored"] = result.bindingsRestored;
        evidence["outcome"] = unsigned(result.outcome);
        if (result.outcome != FSRD::CyberpunkEngineAccess::Outcome::PrivateRecordedRestored)
            throw std::runtime_error("private guide recording refused/failed; no guide companions published");
        plan.earlyWork = std::move(work);
        for (size_t i = 0; i < 3; ++i)
        {
            plan.layers.earlyGuides[i].resource = plan.earlyWork->Outputs()[i];
            plan.layers.earlyGuides[i].viewFormat = i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
            plan.layers.earlyGuides[i].state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        evidence["status"] = "private_dispatch_recorded";
    }
    catch (const std::exception& error)
    {
        evidence["reason"] = error.what();
        LOG_WARN("[FSRRR private guides] one-shot refused: {}", error.what());
    }
}

struct LightingCapturePlan
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<IUnknown> listIdentity;
    std::array<ComPtr<ID3D12Resource>, 4> resources;
    std::array<FSRD::CyberpunkEngineAccess::TextureBorrow, 4> textures {};
    std::array<BoundRtv, 2> targets;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
    ListState drawState;
    uintptr_t list = 0, context = 0, view = 0;
    uint64_t serial = 0;
    Json metadata, bindings, provenance;
    std::shared_ptr<FSRD::CyberpunkGuidePass::Work> work;
};

// Read the actual pixel binding-cache route consumed by authenticated1f22e4.
// This does not guess native root indices from range indices and does not treat
// a positive registry ref or a remembered descriptor as proof of current use.
Json ObserveLightingBindings(uintptr_t list, uintptr_t pso, const Json& metadata)
{
    uintptr_t tls = 0, engine = 0, cache = 0, layout = 0, descriptors = 0, native = 0, cachedPso = 0;
    uint8_t initialized = 0;
    const auto slots = uintptr_t(__readgsqword(0x58));
    if (!ReadEarly(slots, tls) || !ReadEarlyAt(tls, 0x14, initialized) || !initialized ||
        !ReadEarlyAt(tls, 0x188, engine) || !ReadEarlyAt(engine, 0x30, native) || native != list ||
        !ReadEarlyAt(engine, 0x3d0, cachedPso) || cachedPso != pso ||
        !ReadEarlyAt(engine, 0x60, cache) || !ReadEarlyAt(cache, 0x68, layout) ||
        !ReadEarlyAt(cache, 0x28, descriptors))
        throw std::runtime_error("current initialized lighting binding cache unavailable");
    Json result = { { "tls", tls }, { "engine", engine }, { "cache", cache }, { "layout", layout },
        { "descriptor_array", descriptors }, { "pixel_srvs", Json::array() } };
    constexpr uint32_t registers[] = { 1, 2, 3, 14 };
    for (size_t i = 0; i < 4; ++i)
    {
        const auto reg = registers[i];
        uint8_t rangeIndex = 0xff;
        std::array<uint8_t, 16> range {};
        uint64_t dirty70 = 0, dirty78 = 0, resourceMask = 0, samplerMask = 0;
        if (!ReadEarlyAt(layout, 0x4c3 + 0x100 + 2 * reg, rangeIndex) || rangeIndex >= 64 ||
            !ReadEarlyAt(layout, 0x38 + uintptr_t(rangeIndex) * 0x10, range) ||
            !ReadEarlyAt(layout, 0x8, resourceMask) || !ReadEarlyAt(layout, 0, samplerMask) ||
            !(resourceMask & (uint64_t(1) << rangeIndex)) || (samplerMask & (uint64_t(1) << rangeIndex)) ||
            !ReadEarlyAt(cache, 0x70, dirty70) || !ReadEarlyAt(cache, 0x78, dirty78) ||
            ((dirty70 | dirty78) & (uint64_t(1) << rangeIndex)))
            throw std::runtime_error("original lighting pixel range missing or not flushed before draw");
        uint32_t base = 0;
        uint16_t first = 0, count = 0;
        std::memcpy(&base, range.data() + 4, 4);
        std::memcpy(&first, range.data() + 8, 2);
        std::memcpy(&count, range.data() + 10, 2);
        const uint64_t index = uint64_t(base) + reg - first;
        // 65536 is our diagnostic read cap, not an inferred engine allocation
        // size. Authenticated current-use/range containment supplies provenance.
        if (reg < first || !count || reg - first >= count || index >= 65536 ||
            range[13] == 2 || range[14] >= 64)
            throw std::runtime_error("original lighting pixel range/descriptor index unsupported");
        uintptr_t descriptor = 0;
        if (!ReadEarlyAt(descriptors, uintptr_t(index) * 8, descriptor) || !descriptor)
            throw std::runtime_error("current lighting CPU binding descriptor unavailable");
        const auto& source = EarlyInput(metadata, i).at("texture_registry").at("descriptor_sources");
        if (source.at("status") != "cpu_descriptor_sources_observed" ||
            !source.at("repeated_source_fields_equal").get<bool>() ||
            descriptor != source.at(i == 3 ? "alternate_cpu_srv_handle" : "ordinary_cpu_srv_handle").get<uintptr_t>())
            throw std::runtime_error("current pixel binding differs from selected authored guide SRV");
        result["pixel_srvs"].push_back({ { "register", reg }, { "range_index", rangeIndex },
            { "range_bytes", range }, { "descriptor_index", index }, { "cpu_srv_handle", descriptor },
            { "native_root_parameter", range[14] }, { "range_dirty70", false }, { "range_dirty78", false } });
    }
    return result;
}

bool LightingDepthAlias(const LightingCapturePlan& plan,
                        const FSRD::CyberpunkEngineAccess::TextureBorrow& texture) noexcept
{
    if (!lightingScope || !lightingScope->hasDsv || lightingScope->dsv.ptr != plan.dsv.ptr ||
        texture.handle != plan.textures[3].handle || texture.native != plan.textures[3].native ||
        !texture.handle || texture.handle > 0x8000 || !plan.dsv.ptr)
        return false;
    uintptr_t registry = 0, native = 0, readonlyDsv = 0;
    int32_t refs = 0;
    uint32_t requestedRead = 0;
    std::array<uint8_t, 12> compact {};
    const auto slot = uintptr_t(0x2f1d8) + uintptr_t(texture.handle - 1) * 0xb0;
    return ReadEarlyAt(authenticatedImage.load(), FSRD::CyberpunkEngineAccess::RegistryRva, registry) &&
        ReadEarlyAt(registry, slot - 8, refs) && refs > 0 &&
        ReadEarlyAt(registry, slot, native) && native == texture.native &&
        ReadEarlyAt(registry, slot + 0x28, readonlyDsv) && readonlyDsv == plan.dsv.ptr &&
        ReadEarlyAt(registry, slot + 0x48, requestedRead) && requestedRead == 0xe0 &&
        ReadEarlyAt(registry, slot + 0x4e, compact) &&
        // Authenticated2221f4 second DSV has Flags3 for these depth/stencil
        // formats. Both planes are read-only, not merely depth-format guessed.
        compact[4] == 1 && compact[5] == 0 && (compact[8] & 4) &&
        (compact[6] & 0xf) == 0 && compact[7] < 0x40 &&
        ((compact[7] & 0x3f) == 0x18 || (compact[7] & 0x3f) == 0x19);
}

struct LightingEngineHost
{
    const LightingCapturePlan& plan;
    bool Read(uintptr_t address, void* destination, size_t bytes) noexcept
    { return ReadExactMemory(address, destination, bytes); }
    uint32_t ThreadId() noexcept { return GetCurrentThreadId(); }
    bool ReadTlsSlotZero(uintptr_t& result) noexcept
    { return ReadEarly(uintptr_t(__readgsqword(0x58)), result) && result; }
    bool ExactImageAuthenticated(uintptr_t image, uintptr_t size, uint32_t stamp, std::string_view sha) noexcept
    {
        return active.load() && captureEnabled.load() && lightingRequested.load() &&
            image == authenticatedImage.load() && image == uintptr_t(GetModuleHandleW(nullptr)) &&
            size == 0x04efc000 && stamp == 0x68af45ea && sha == ExeSha256;
    }
    bool LiveCodeMatches(uintptr_t image, const FSRD::CyberpunkEngineAccess::CodeRange& code) noexcept
    { return MatchLiveCode(image, code); }
    bool IsReadOnlyDepthAliasAdmitted(const FSRD::CyberpunkEngineAccess::TextureBorrow& texture) noexcept
    { return LightingDepthAlias(plan, texture); }
    bool IsAdmittedFogScope(uint64_t serial, uintptr_t list, uintptr_t pso,
                            const std::array<FSRD::CyberpunkEngineAccess::TextureBorrow, 4>& inputs) noexcept
    {
        if (!lightingScope || !lightingScope->finalHelper || lightingScope->serial != plan.serial || serial != plan.serial ||
            uintptr_t(lightingScope->context) != plan.context || list != plan.list ||
            lightingScope->psoList != reinterpret_cast<ID3D12GraphicsCommandList*>(list) ||
            lightingScope->pso.Get() != plan.pso.Get() || pso != uintptr_t(plan.pso.Get()) ||
            lightingScope->rtvList != reinterpret_cast<ID3D12GraphicsCommandList*>(list) || lightingScope->rtvCount != 2 ||
            !captureTrackingValid.load() || !LightingDepthAlias(plan, inputs[3]))
            return false;
        try
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            const auto found = data.lists.find(plan.listIdentity.Get());
            if (found == data.lists.end() || found->second.generation != plan.drawState.generation ||
                !found->second.known || found->second.predicated || found->second.renderPass || found->second.queryCount)
                return false;
        }
        catch (...) { return false; }
        uintptr_t view = 0;
        uint8_t flags = 0;
        if (!ReadEarlyAt(plan.context, 0x18, view) || view != plan.view ||
            !ReadEarlyAt(plan.context, 0x30, flags) || !(flags & 2)) return false;
        for (size_t i = 0; i < 4; ++i)
        {
            if (inputs[i].handle != plan.textures[i].handle || inputs[i].native != plan.textures[i].native || !plan.resources[i])
                return false;
            for (size_t target = 0; target < 2; ++target)
                if (lightingScope->targets[target].resource.Get() != plan.targets[target].resource.Get() ||
                    inputs[i].native == uintptr_t(plan.targets[target].resource.Get())) return false;
        }
        return true;
    }
    bool ListIsDirect(uintptr_t list) noexcept
    { return reinterpret_cast<ID3D12GraphicsCommandList*>(list)->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT; }
    uintptr_t CurrentNativeList(uintptr_t address) noexcept
    { return uintptr_t(reinterpret_cast<void*(__fastcall*)()>(address)()); }
    void RequestState(uintptr_t address, uintptr_t engine, uint32_t handle, uint32_t state, uint32_t subresource) noexcept
    { reinterpret_cast<void(__fastcall*)(void*, uint32_t, uint32_t, uint32_t)>(address)(reinterpret_cast<void*>(engine), handle, state, subresource); }
    void Flush(uintptr_t address, uintptr_t engine) noexcept
    { reinterpret_cast<void(__fastcall*)(void*)>(address)(reinterpret_cast<void*>(engine)); }
    void Reenter(uintptr_t address, uintptr_t list) noexcept
    { reinterpret_cast<void(__fastcall*)(ID3D12GraphicsCommandList*)>(address)(reinterpret_cast<ID3D12GraphicsCommandList*>(list)); }
    void RestorePso(uintptr_t list, uintptr_t pso) noexcept
    { originalSetPso(reinterpret_cast<ID3D12GraphicsCommandList*>(list), reinterpret_cast<ID3D12PipelineState*>(pso)); }
};

std::shared_ptr<FSRD::CyberpunkGuidePass::Work> PrepareLightingGuideWork(LightingCapturePlan& plan)
{
    const auto& current = plan.metadata;
    const auto dimensions = current.at("view_dimensions").get<std::array<uint32_t, 2>>();
    FSRD::CyberpunkGuideMatrix::MatrixWords inverseProjection {}, inverseView {};
    const auto& matrices = current.at("camera_provenance").at("matrices");
    for (const auto& item : { std::pair { "inverse_native_projection_jittered", &inverseProjection },
                              std::pair { "inverse_native_view", &inverseView } })
    {
        const auto rows = matrices.at(item.first).at("source_uint32_rows").get<std::array<std::array<uint32_t, 4>, 4>>();
        std::memcpy(item.second->data(), rows.data(), 64);
    }
    FSRD::CyberpunkGuideConstants::ObservedSharedWords sharedWords {};
    if (!FSRD::CyberpunkGuideMatrix::Generate(inverseProjection, inverseView, dimensions[0], dimensions[1], sharedWords))
        throw std::runtime_error("exact current native camera recipe unavailable");
    const auto& settings = current.at("guide_settings");
    if (settings.at("extra_specular_enabled").get<unsigned>() != 0)
        throw std::runtime_error("authored extra-specular branch needs unavailable t5; not disabled implicitly");
    FSRD::CyberpunkGuideConstants::PassSources passSources;
    passSources.width = dimensions[0]; passSources.height = dimensions[1];
    passSources.transparency = FSRD::CyberpunkGuideConstants::TransparencyInput::PreTransparencySurface;
    passSources.noVMode = settings.at("NoV_mode").get<int32_t>();
    passSources.extraSpecularScaleBits = settings.at("extra_specular_scale_bits").get<uint32_t>();
    FSRD::CyberpunkGuideConstants::PassConstants pass {};
    if (!FSRD::CyberpunkGuideConstants::PackPass(passSources, pass))
        throw std::runtime_error("exact authored pass constants unavailable");
    std::array<FSRD::CyberpunkGuidePass::SourceView, 4> sources;
    Json views = Json::array();
    for (size_t i = 0; i < 4; ++i)
    {
        const auto& descriptors = EarlyInput(current, i).at("texture_registry").at("descriptor_sources");
        if (i == 3)
        {
            const auto formatTag = descriptors.at("raw_format_sample_bits").get<uint32_t>() & 0x3f;
            if (descriptors.at("raw_array_size").get<uint32_t>() != 1 ||
                (descriptors.at("raw_dimension_mip_bits").get<uint32_t>() & 0xf) != 0 ||
                (descriptors.at("raw_flags_bits").get<uint32_t>() & 5) != 5 ||
                (formatTag != 0x18 && formatTag != 0x19) ||
                plan.resources[i]->GetDesc().Format !=
                    (formatTag == 0x18 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS))
                throw std::runtime_error("current t4 does not satisfy authenticated stencil-view factory branch");
        }
        const auto descriptor = descriptors.at(i == 3 ? "alternate_cpu_srv_handle" : "ordinary_cpu_srv_handle").get<SIZE_T>();
        sources[i].resource = plan.resources[i];
        sources[i].descriptor.ptr = descriptor;
        uint64_t generation = 0;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (const auto& heap : data.cpuSrvHeaps)
                if (descriptor >= heap.start && (descriptor - heap.start) % heap.increment == 0 &&
                    (descriptor - heap.start) / heap.increment < heap.count)
                {
                    if (sources[i].heap) throw std::runtime_error("current source CPU range ambiguous");
                    sources[i].heap = heap.heap;
                    generation = heap.generation;
                }
        }
        if (!sources[i].heap) throw std::runtime_error("current authored SRV has no retained CPU-only heap");
        views.push_back({ { "private_register", i == 3 ? 4 : i }, { "lighting_register", i == 3 ? 14 : i + 1 },
            { "handle", plan.textures[i].handle }, { "native", plan.textures[i].native },
            { "cpu_srv_handle", descriptor }, { "heap_generation", generation }, { "descriptor_transform", "none" } });
    }
    std::vector<std::byte> shader;
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        shader = data.guideShader;
    }
    if (shader.size() != GuideShaderBytes) throw std::runtime_error("runtime authenticated guide shader unavailable");
    if (ObserveLightingBindings(plan.list, uintptr_t(plan.pso.Get()), current) != plan.bindings)
        throw std::runtime_error("current authored binding cache changed before descriptor copies");
    auto work = FSRD::CyberpunkGuidePass::Prepare(plan.device.Get(), dimensions[0], dimensions[1], sources, pass,
        FSRD::CyberpunkGuideConstants::PackShared(sharedWords), shader);
    if (!work) throw std::runtime_error("private guide resource/PSO setup refused");
    plan.provenance["sources"] = std::move(views);
    plan.provenance["cb12_words"] = sharedWords;
    plan.provenance["cb6_words"] = pass;
    plan.provenance["shader_sha256"] = GuideShaderSha256;
    return work;
}

std::shared_ptr<LightingCapturePlan> PrepareLightingCapture(ID3D12GraphicsCommandList* list,
                                                           std::shared_ptr<LightingCapturePlan>& diagnostic)
{
    auto plan = std::make_shared<LightingCapturePlan>();
    diagnostic = plan; // Refusals retain bounded evidence even when this function throws.
    auto& current = *lightingScope;
    plan->list = uintptr_t(list); plan->context = uintptr_t(current.context); plan->serial = current.serial;
    plan->provenance = { { "schema", "optiscaler.fsr_rr.lighting_private_guides.v1" }, { "status", "preparing" },
        { "original_scene_modified", false }, { "node_rva", LightingNodeRva },
        { "helper_return_rva", FinalLightingHelperReturnRva }, { "native_draw_return_rva", NativeFullscreenDrawReturnRva },
        { "transparent_input", "explicit_pre_transparency_t3_t6_disabled" }, { "extra_specular_t5", "requires_authored_disabled" },
        { "SL_frame_association", "not_asserted" }, { "GPU_completion", "requires_standalone_completion_fence" } };
    plan->provenance["draw_observation"] = { { "native_list", plan->list }, { "scope", plan->serial },
        { "pso_list", uintptr_t(current.psoList) }, { "pso", uintptr_t(current.pso.Get()) },
        { "rtv_list", uintptr_t(current.rtvList) }, { "rtv_count", current.rtvCount },
        { "has_dsv", current.hasDsv }, { "dsv", current.dsv.ptr } };
    uint8_t flags = 0;
    uint32_t nodeKind = 0;
    if (!ReadEarlyAt(plan->context, 0x30, flags) || !(flags & 2) ||
        !ReadEarlyAt(uintptr_t(current.node), 0x18, nodeKind) || nodeKind != 2 ||
        current.psoList != list || !current.pso || current.rtvList != list || current.rtvCount != 2 ||
        !current.hasDsv || !current.dsv.ptr || !earlyHeapTrackingValid.load())
        throw std::runtime_error("final lighting kind/PSO/two-MRT/read-only-DSV observation incomplete");
    plan->pso = current.pso; plan->targets = current.targets; plan->dsv = current.dsv;
    plan->listIdentity = ListIdentity(list);
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        const auto found = data.lists.find(plan->listIdentity.Get());
        if (found == data.lists.end() || !found->second.known || found->second.predicated ||
            found->second.renderPass || found->second.queryCount)
            throw std::runtime_error("final lighting Reset/predication/query/render-pass state unavailable");
        plan->drawState = found->second;
    }
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&plan->device)))) throw std::runtime_error("lighting device unavailable");
    plan->metadata = Json::parse(FSRDCyberpunkEarlyGuides::Describe(current.context, authenticatedImage.load()));
    plan->provenance["current_inputs"] = plan->metadata;
    plan->view = plan->metadata.at("view").get<uintptr_t>();
    const auto dimensions = plan->metadata.at("view_dimensions").get<std::array<uint32_t, 2>>();
    for (const auto& target : plan->targets)
    {
        if (!target.known || !target.resource || target.view.ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D ||
            target.view.Texture2D.MipSlice || target.view.Texture2D.PlaneSlice)
            throw std::runtime_error("both original MRT resource identities must be owned at their native bind");
        const auto desc = target.resource->GetDesc();
        if (desc.Width != dimensions[0] || desc.Height != dimensions[1])
            throw std::runtime_error("original lighting MRT extents differ from current view");
    }
    plan->bindings = ObserveLightingBindings(plan->list, uintptr_t(plan->pso.Get()), plan->metadata);
    plan->provenance["actual_pixel_bindings"] = plan->bindings;
    UINT64 ownedBytes = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        const auto& source = EarlyInput(plan->metadata, i);
        const auto& registry = source.at("texture_registry");
        plan->textures[i] = { source.at("handle").get<uint32_t>(), registry.at("borrowed_native_address").get<uintptr_t>() };
        if (!plan->textures[i].native || registry.at("descriptor_sources").at("requested_srv_state_mask").get<uint32_t>() !=
                (i == 3 ? 0xe0u : 0xc0u))
            throw std::runtime_error("current authored resource/read-state identity unavailable");
        for (const auto& target : plan->targets)
            if (plan->textures[i].native == uintptr_t(target.resource.Get()))
                throw std::runtime_error("guide source aliases original writable lighting MRT");
        // Exact authenticated final native Draw consumes these selected current
        // pixel SRVs NOW. Its active graph/cache borrow, not old clears or sampled
        // pointers, is the ownership boundary for this AddRef.
        plan->resources[i] = reinterpret_cast<ID3D12Resource*>(plan->textures[i].native);
        const auto desc = plan->resources[i]->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != dimensions[0] ||
            desc.Height != dimensions[1] || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
            throw std::runtime_error("current lighting guide layout unsupported");
        const auto bytes = plan->device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        constexpr UINT64 MaxOwnedInputBytes = 128ull * 1024 * 1024;
        if (!bytes || bytes > MaxOwnedInputBytes - ownedBytes) throw std::runtime_error("lighting input ownership budget exceeded");
        ownedBytes += bytes;
    }
    if (!LightingDepthAlias(*plan, plan->textures[3]))
        throw std::runtime_error("actual bound DSV differs from authenticated both-planes-readonly t14 alias");
    const auto fresh = Json::parse(FSRDCyberpunkEarlyGuides::Describe(current.context, authenticatedImage.load()));
    for (size_t i = 0; i < 4; ++i)
        if (!SameEarlyReservation(plan->metadata, fresh, i) ||
            EarlyInput(plan->metadata, i).at("texture_registry").at("descriptor_sources") !=
                EarlyInput(fresh, i).at("texture_registry").at("descriptor_sources"))
            throw std::runtime_error("current lighting reservation/descriptor changed during preparation");
    if (fresh.at("view") != plan->metadata.at("view") || fresh.at("view_dimensions") != plan->metadata.at("view_dimensions") ||
        fresh.at("guide_settings") != plan->metadata.at("guide_settings") ||
        fresh.at("camera_provenance").at("matrices") != plan->metadata.at("camera_provenance").at("matrices"))
        throw std::runtime_error("current lighting camera/settings changed during preparation");
    plan->work = PrepareLightingGuideWork(*plan);
    plan->provenance["current_inputs"] = plan->metadata;
    plan->provenance["actual_pixel_bindings"] = plan->bindings;
    plan->provenance["native_list"] = plan->list;
    plan->provenance["recording_generation"] = plan->drawState.generation;
    plan->provenance["scope"] = plan->serial;
    plan->provenance["original_mrt_resources"] = { uintptr_t(plan->targets[0].resource.Get()), uintptr_t(plan->targets[1].resource.Get()) };
    plan->provenance["readonly_dsv"] = plan->dsv.ptr;
    return plan;
}

void FinishLightingCapture(ID3D12GraphicsCommandList* list, const std::shared_ptr<LightingCapturePlan>& plan)
{
    // Original final lighting DrawInstanced has already returned exactly once.
    // Compute does not touch OM/RS/IA: retain both original MRTs and read-only DSV
    // bindings unchanged. Engine state requests preserve the DSV DEPTH_READ bit.
    if (ObserveLightingBindings(plan->list, uintptr_t(plan->pso.Get()), plan->metadata) != plan->bindings)
        throw std::runtime_error("original draw changed current lighting binding identity");
    LightingEngineHost host { *plan };
    FSRD::CyberpunkEngineAccess::Input input;
    input.image = authenticatedImage.load(); input.list = plan->list; input.originalPso = uintptr_t(plan->pso.Get());
    input.originalFogScope = plan->serial; input.textures = plan->textures; input.preserveReadOnlyDepth = true;
    if (!FSRDSubmission::Retain(plan->device.Get(), list, plan))
        throw std::runtime_error("lighting capture lifetime retention unavailable");
    const auto result = FSRD::CyberpunkEngineAccess::RecordPrivateCompute(host, input, [&] { return plan->work->Record(list); });
    if (result.outcome == FSRD::CyberpunkEngineAccess::Outcome::ScopeLostAfterPrivate)
    {
        earlyFatalRecording.store(true); // Nonthrowing latch precedes every log/JSON allocation.
        try { LOG_ERROR("[FSRRR lighting guides] FATAL private recording lost original scope; terminating authenticated Cyberpunk only"); }
        catch (...) {}
        if (active.load() && captureEnabled.load() && lightingRequested.load() &&
            input.image == authenticatedImage.load() && input.image == uintptr_t(GetModuleHandleW(nullptr)))
        {
            TerminateProcess(GetCurrentProcess(), 0xf51d0001u);
            RaiseFailFastException(nullptr, nullptr, 0);
        }
        return;
    }
    plan->provenance["engine_state_requests"] = result.requestsIssued;
    plan->provenance["bindings_restored"] = result.bindingsRestored;
    plan->provenance["outcome"] = unsigned(result.outcome);
    if (result.outcome != FSRD::CyberpunkEngineAccess::Outcome::PrivateRecordedRestored)
        throw std::runtime_error("private lighting guide recording refused/failed; no completed guide payload");
    plan->provenance["status"] = "private_dispatch_recorded";
    plan->provenance["recording_position"] = "after original final lighting draw on its own native list/Reset";
    plan->provenance["read_states"] = { 0xc0, 0xc0, 0xc0, 0xe0 };
    std::array<FSRDFogLayerCapture::Texture, 3> outputs;
    for (size_t i = 0; i < 3; ++i)
    {
        outputs[i].resource = plan->work->Outputs()[i];
        outputs[i].viewFormat = i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        outputs[i].state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    const bool recorded = FSRDFogLayerCapture::RecordEarlyGuides(plan->device.Get(), list, outputs, plan->provenance.dump(), plan);
    LOG_INFO("[FSRRR lighting guides] original draw preserved; private guide readback recorded={} scope={}", recorded, plan->serial);
}

bool HasBoundCb12Psos(const CapturePlan& plan)
{
    return std::all_of(plan.boundCb12Psos.begin(), plan.boundCb12Psos.end(),
                       [](const auto& pso) { return bool(pso); });
}

void PrepareBoundCb12Target(CapturePlan& plan, UINT64 remainingBytes)
{
    if (!HasBoundCb12Psos(plan))
        return;
    try
    {
        if (!originalSetViewports || !originalSetScissors)
            throw std::runtime_error("exact viewport/scissor restore hooks unavailable");
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_UINT, 5, 1, 1, 1,
                                                       1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto bytes = plan.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (!bytes || bytes > remainingBytes)
            throw std::runtime_error("bound cb12 target exceeds remaining private texture budget");
        const CD3DX12_HEAP_PROPERTIES properties(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(plan.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&plan.layers.boundCb12.resource))))
            throw std::runtime_error("bound cb12 UINT target allocation failed");
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        if (FAILED(plan.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&plan.boundCb12Heap))))
            throw std::runtime_error("bound cb12 private RTV allocation failed");
        plan.boundCb12Rtv = plan.boundCb12Heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC view {};
        view.Format = desc.Format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        plan.device->CreateRenderTargetView(plan.layers.boundCb12.resource.Get(), &view, plan.boundCb12Rtv);
        plan.layers.boundCb12.viewFormat = desc.Format;
        plan.layers.boundCb12.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        plan.provenance["bound_cb12_probe"]["status"] = "private_target_prepared";
        plan.provenance["bound_cb12_probe"]["private_allocation_bytes"] = bytes;
    }
    catch (const std::exception& error)
    {
        // No commands reference this optional target yet. Preserve the original
        // three-layer capture when compiler/device/format/allocation support fails.
        plan.layers.boundCb12 = {};
        plan.boundCb12Heap.Reset();
        for (auto& pso : plan.boundCb12Psos) pso.Reset();
        plan.provenance["bound_cb12_probe"]["status"] = "unavailable";
        plan.provenance["bound_cb12_probe"]["reason"] = error.what();
    }
}

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
        plan->drawState = state;
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
            RefuseCapture("exact mip0 RGBA16F RTV descriptor provenance unavailable");
            return {};
        }
        // Already owned since the original OM bind; do not resolve its CPU handle
        // again. The game may legally have reused that descriptor in the meantime.
        plan->main = bound.resource;
        plan->sourceHeap = bound.heap;
        plan->originalView = bound.view;
        plan->originalPso = foundPso->pso;
        plan->authoredPso = foundPso->authored;
        for (size_t i = 0; i < BoundCb12Registers.size(); ++i)
            plan->boundCb12Psos[i] = foundPso->boundCb12[i].pso;
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
                { "descriptor_source", bound.documentedDefault ? "documented typed RGBA16F default" : "explicit RTV" },
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
            { "bound_cb12_probe", {
                { "schema", "optiscaler.fsr_rr.bound_cb12_probe.v2" },
                { "status", HasBoundCb12Psos(*plan) ? "private_pso_ready" : "unavailable" },
                { "scope", "High-only exact inherited graphics binding; no engine-frame assertion" },
                { "shader_variant", foundPso->shader }, { "cb_register", 12 }, { "register_space", 0 },
                { "register_indices", { 21, 22, 23, 24, 27 } }, { "output_size", { 5, 1 } },
                { "output_format", UINT(DXGI_FORMAT_R32G32B32A32_UINT) },
                { "generated_ps_source_template", BoundCb12Shader }, { "generated_ps_target", "ps_5_0" },
                { "generated_ps_entry", "PSMain" }, { "compiler_flags", "D3DCOMPILE_OPTIMIZATION_LEVEL3" },
                { "generated_ps_template_sha256", foundPso->cb12TemplateSha256 },
                { "generated_ps_variants", Json::array() },
                { "draw_count", BoundCb12Registers.size() },
                { "selection", "fixed register per PS/draw; one-pixel scissor column" },
                { "shading_rate", "inherited unchanged; output constant for every invocation in each draw" },
                { "original_ps_sha256", foundPso->pixelSha256 }, { "original_vs_sha256", FogVertexSha256 },
                { "inherited_bindings", true }, { "value_transform", "none; native uint32 bits" },
                { "graphics_state_restore", "original PSO, frozen original RTV, exact viewport/scissor arrays" }
            } },
        };
        for (size_t i = 0; i < BoundCb12Registers.size(); ++i)
        {
            const auto& variant = foundPso->boundCb12[i];
            plan->provenance["bound_cb12_probe"]["generated_ps_variants"].push_back({
                { "register_index", BoundCb12Registers[i] }, { "output_column", i },
                { "scissor_rect", { i, 0, i + 1, 1 } },
                { "source", variant.source }, { "source_sha256", variant.sourceSha256 },
                { "bytecode_sha256", variant.bytecodeSha256 }
            });
        }
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
    Json earlyAvailability;
    if (earlyRequested.load())
    {
        earlyAvailability = Json::parse(FSRDCyberpunkEarlyGuides::Describe(s.context, authenticatedImage.load()));
        std::shared_ptr<EarlyProducer> producer;
        bool hasShader = false;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            producer = data.earlyProducer;
            hasShader = data.guideShader.size() == GuideShaderBytes;
        }
        if ((!producer || !ProducerMatches(*producer, earlyAvailability, list, state.generation) || !hasShader) &&
            GetTickCount64() - earlyRequestedAt.load() < 10000)
        {
            RefuseCapture("waiting for current original GBuffer clears on same Fog recording and authenticated guide shader (10s maximum)");
            return {};
        }
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
    SYSTEMTIME now {};
    GetSystemTime(&now);
    plan->endpoint->startedAt = GetTickCount64();
    plan->endpoint->sessionKey = std::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}Z-{}-{}", now.wYear,
        now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        GetCurrentProcessId(), plan->provenance["scope_serial"].get<uint64_t>());
    plan->endpoint->sidecarRelative = std::format("FSRRR-fog-captures/provenance-{}.json", plan->endpoint->sessionKey);
    plan->endpoint->fog = {
        { "session_key", plan->endpoint->sessionKey }, { "process_id", GetCurrentProcessId() },
        { "submission_sidecar", plan->endpoint->sidecarRelative },
        { "scope_serial", plan->provenance["scope_serial"] },
        { "command_list_identity", std::format("{:x}", uintptr_t(plan->endpoint->list.Get())) },
        { "command_list_generation", plan->endpoint->generation }, { "endpoint_ordinal", plan->endpoint->ordinal },
        { "ordinal_scope", "observed endpoints within this Reset recording only" },
        { "resource", OwnEndpointResource(*plan->endpoint, plan->main.Get()) },
        { "subresource", 0 }, { "view_format", UINT(plan->originalView.Format) },
        { "rr_frame_association", "not_established" }
    };
    plan->provenance["endpoint_origin"] = plan->endpoint->fog;
    // One accepted, authenticated capture only. CPU table availability does not
    // establish initialized GPU contents or authorize an early denoiser dispatch.
    plan->provenance["early_guide_availability"] = earlyAvailability.is_object() ? std::move(earlyAvailability) :
        Json::parse(FSRDCyberpunkEarlyGuides::Describe(s.context, uintptr_t(GetModuleHandleW(nullptr))));
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
    PrepareBoundCb12Target(*plan, MaxCaptureTextureBytes - (mainBytes + 2 * copyBytes + authoredBytes));

    // Retain BEFORE the first private-copy command. This independent ticket keeps
    // earlier work alive even if the later readback helper refuses or throws.
    FSRDSubmission::Retain(plan->device.Get(), list, plan);
    CopyMain(list, *plan, plan->layers.before.resource.Get());
    PrepareAndRecordEarlyGuides(list, *plan); // Optional private outputs; never writes the original scene.
    return plan;
}

void PublishFogEndpoint(const std::shared_ptr<CapturePlan>& plan) noexcept
{
    Metadata([&] {
        auto& data = Data();
        {
            std::lock_guard lock(data.mutex);
            data.endpoint = plan->endpoint;
            data.submissionTrace = plan->endpoint;
            endpointActive.store(true, std::memory_order_release);
            submissionActive.store(true, std::memory_order_release);
        }
        LOG_INFO("[FSRRR fog endpoint] original fog draw recorded; origin={}; next {} CPU-observed RR endpoints only; no GPU/frame/content association",
                 plan->endpoint->fog.dump(), MaxNgxEndpoints);
    });
}

void CaptureBoundCb12(ID3D12GraphicsCommandList* list, CapturePlan& plan)
{
    if (!HasBoundCb12Psos(plan) || !plan.layers.boundCb12.resource)
        return;
    {
        struct RestoreConstantProbeState
        {
            ID3D12GraphicsCommandList* list;
            const CapturePlan& plan;
            ~RestoreConstantProbeState()
            {
                originalSetPso(list, plan.originalPso.Get());
                originalSetRtv(list, 1, &plan.frozenOriginalRtv, plan.contiguous, nullptr);
                originalSetViewports(list, plan.drawState.viewportCount, plan.drawState.viewports.data());
                originalSetScissors(list, plan.drawState.scissorCount, plan.drawState.scissors.data());
            }
        } restore { list, plan };
        const FLOAT clear[] = { 0, 0, 0, 0 };
        list->ClearRenderTargetView(plan.boundCb12Rtv, clear, 0, nullptr);
        const D3D12_VIEWPORT viewport { 0, 0, 5, 1, 0, 1 };
        // Keep both root signatures, all root arguments, heaps and input bindings
        // untouched. Every private PSO retains the authenticated original VS.
        originalSetRtv(list, 1, &plan.boundCb12Rtv, FALSE, nullptr);
        originalSetViewports(list, 1, &viewport);
        for (size_t i = 0; i < BoundCb12Registers.size(); ++i)
        {
            // VRS changes invocation frequency, not scissor pixel coverage. Even
            // if the invocation lies outside this column, its constant uint4 is
            // correct for every covered sample. No VRS state changes are needed.
            const D3D12_RECT scissor { LONG(i), 0, LONG(i + 1), 1 };
            originalSetPso(list, plan.boundCb12Psos[i].Get());
            originalSetScissors(list, 1, &scissor);
            originalDraw(list, 3, 1, 0, 0);
        }
    }
    // JSON/log allocations happen only after exact graphics state restoration.
    plan.provenance["bound_cb12_probe"]["status"] = "private_draws_recorded";
    plan.provenance["bound_cb12_probe"]["recording_position"] = "same fog scope/list after original and private authored draw";
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
    CaptureBoundCb12(list, *plan);
    const bool recorded = FSRDFogLayerCapture::Record(plan->device.Get(), list, plan->layers,
                                                     plan->provenance.dump(), plan);
    {
        const auto status = FSRDFogLayerCapture::GetStatus();
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        plan->endpoint->fogRecorded = recorded;
        if (!status.directory.empty())
            plan->endpoint->fogCaptureId = std::filesystem::path(status.directory).filename().string();
        if (!recorded)
            plan->endpoint->failure = "native fog readback was not recorded";
    }
    FinalizeSubmissionTrace(plan->endpoint);
    LOG_INFO("[FSRRR fog capture] original draw preserved; private readback recorded={} scope={}",
             recorded, plan->provenance["scope_serial"].get<uint64_t>());
}

bool MatchesFinalLightingDraw(bool scoped, bool finalHelper, uintptr_t nativeCaller, uintptr_t image,
                              UINT count, UINT instances, UINT start, UINT firstInstance) noexcept
{
    return scoped && finalHelper && image && image <= UINTPTR_MAX - NativeFullscreenDrawReturnRva &&
        nativeCaller == image + NativeFullscreenDrawReturnRva && count == 3 && instances == 1 && !start && !firstInstance;
}

void WINAPI HookDraw(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT start, UINT firstInstance)
{
    const auto nativeCaller = uintptr_t(_ReturnAddress());
    LogDraw(list, false, count, instances, start, 0, firstInstance);
    std::shared_ptr<LightingCapturePlan> lightingPlan;
    if (captureEnabled.load() && !inMetadata && lightingRequested.load() && !lightingAttempted.load() &&
        FSRDFogLayerCapture::WantsEarlyGuideCapture() &&
        MatchesFinalLightingDraw(lightingScope != nullptr, lightingScope && lightingScope->finalHelper,
            nativeCaller, authenticatedImage.load(), count, instances, start, firstInstance) &&
        !lightingAttempted.exchange(true))
        Metadata([&] {
            std::shared_ptr<LightingCapturePlan> diagnostic;
            try
            {
                ++lightingScope->finalDraws;
                if (lightingScope->finalDraws != 1) throw std::runtime_error("ambiguous repeated final lighting draw");
                lightingPlan = PrepareLightingCapture(list, diagnostic);
            }
            catch (const std::exception& error)
            {
                FSRDFogLayerCapture::CancelEarlyGuideRequest();
                if (diagnostic)
                {
                    diagnostic->provenance["status"] = "refused_before_original_draw";
                    diagnostic->provenance["reason"] = error.what();
                    LOG_WARN("[FSRRR lighting guides] refusal {}", diagnostic->provenance.dump());
                }
                else LOG_WARN("[FSRRR lighting guides] preparation refused: {}", error.what());
            }
        });
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
    if (earlyFatalRecording.load() || (plan && plan->fatalEarlyRecording))
        return; // Only the fatal post-private scope-loss outcome; never ordinary refusal.
    originalDraw(list, count, instances, start, firstInstance);
    if (lightingPlan)
        Metadata([&] {
            try { FinishLightingCapture(list, lightingPlan); }
            catch (const std::exception& error)
            {
                FSRDFogLayerCapture::CancelEarlyGuideRequest();
                lightingPlan->provenance["status"] = "refused_after_original_draw";
                lightingPlan->provenance["reason"] = error.what();
                LOG_WARN("[FSRRR lighting guides] refusal {}", lightingPlan->provenance.dump());
            }
        });
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

void PrepareBoundCb12Pso(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& compatible,
                         TaggedPso& tagged) noexcept
{
    try
    {
        // Accesses are proven only for High. Other exact fog variants retain the
        // existing three-layer capture without this optional companion.
        if (!captureEnabled.load() || !tagged.authored || tagged.pixelSha256 != FogShaders[0].sha256 ||
            !(compatible.SampleMask & 1))
            return;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { DXGI_FORMAT_R32G32B32A32_UINT };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
            return;
        Hash templateHash(Data().crypto);
        templateHash.Add(BoundCb12Shader, ULONG(sizeof(BoundCb12Shader) - 1));
        const auto templateSha256 = templateHash.Finish();
        if (templateSha256.empty()) return;
        // Publish only a complete five-PSO set. Partial compilation/device failure
        // leaves the existing three-layer capture intact and no companion active.
        std::array<BoundCb12Variant, BoundCb12Registers.size()> variants;
        for (size_t i = 0; i < BoundCb12Registers.size(); ++i)
        {
            auto& variant = variants[i];
            variant.source = std::format("#define BOUND_CB12_REGISTER {}\n{}", BoundCb12Registers[i], BoundCb12Shader);
            ComPtr<ID3DBlob> code, errors;
            const auto compiled = D3DCompile(variant.source.data(), variant.source.size(), nullptr, nullptr, nullptr,
                "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(compiled) || !code)
            {
                LOG_WARN("[FSRRR fog cb12] optional High companion shader compile failed register={}: {:x}",
                         BoundCb12Registers[i], UINT(compiled));
                return;
            }
            Hash sourceHash(Data().crypto), bytecodeHash(Data().crypto);
            sourceHash.Add(variant.source.data(), ULONG(variant.source.size()));
            bytecodeHash.Add(code->GetBufferPointer(), ULONG(code->GetBufferSize()));
            variant.sourceSha256 = sourceHash.Finish();
            variant.bytecodeSha256 = bytecodeHash.Finish();
            if (variant.sourceSha256.empty() || variant.bytecodeSha256.empty()) return;
            auto clone = compatible;
            clone.PS = { code->GetBufferPointer(), code->GetBufferSize() };
            clone.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
            clone.CachedPSO = {};
            const auto created = originalCreateGraphics(device, &clone, IID_PPV_ARGS(&variant.pso));
            LOG_INFO("[FSRRR fog cb12] optional High UINT companion PSO result={:x}; shader={} register={} column={}",
                     UINT(created), variant.bytecodeSha256, BoundCb12Registers[i], i);
            if (FAILED(created) || !variant.pso) return;
        }
        tagged.cb12TemplateSha256 = templateSha256;
        tagged.boundCb12 = std::move(variants);
    }
    catch (...)
    {
        // Compilation/metadata failures must not disable the existing fog capture.
        for (auto& variant : tagged.boundCb12) variant = {};
        tagged.cb12TemplateSha256.clear();
    }
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
        PrepareBoundCb12Pso(device.Get(), clone, tagged);
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

void RecordGuideShader(const D3D12_SHADER_BYTECODE& shader)
{
    if (!captureEnabled.load() || shader.BytecodeLength != GuideShaderBytes || !shader.pShaderBytecode) return;
    std::vector<std::byte> bytes(GuideShaderBytes);
    if (!ReadExactMemory(uintptr_t(shader.pShaderBytecode), bytes.data(), bytes.size())) return;
    Hash hash(Data().crypto);
    hash.Add(bytes.data(), ULONG(bytes.size()));
    if (hash.Finish() != GuideShaderSha256) return;
    auto& data = Data();
    std::lock_guard lock(data.mutex);
    if (data.guideShader.empty())
    {
        data.guideShader = std::move(bytes);
        LOG_INFO("[FSRRR private guides] exact original runtime guide CS retained: {} bytes SHA256={}", GuideShaderBytes, GuideShaderSha256);
    }
}

HRESULT WINAPI HookCreateCompute(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                 REFIID iid, void** result)
{
    const HRESULT hr = originalCreateCompute(device, desc, iid, result);
    if (SUCCEEDED(hr) && desc && result && *result)
        Metadata([&] { RecordGuideShader(desc->CS); });
    return hr;
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
            RecordGuideShader(D3D12_SHADER_BYTECODE(parsed.PipelineStream.CS));
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

CaptureCandidate ObserveNgxInput(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
                     ID3D12Resource* colorBeforeParticles, uint64_t featureId, uint64_t frameIndex,
                     UINT renderWidth, UINT renderHeight) noexcept
{
    if (!endpointActive.load(std::memory_order_acquire))
        return {}; // No COM calls, allocation, file polling or locks on the inactive path.
    const auto slEvaluation = FSRD::SlEvaluationProvenance::Current();
    CaptureCandidate result;
    Metadata([&] {
        std::shared_ptr<EndpointTrace> trace;
        try
        {
            // The real public SL token is useful only when this NGX call is
            // synchronously nested in that invocation. No global-last fallback,
            // pointer canonicalization claim, or Fog frame association follows.
            const Json slProvenance = {
                { "schema", "optiscaler.fsr_rr.sl_evaluation_scope.v1" },
                { "observed", slEvaluation.observed },
                { "frame_index", slEvaluation.observed ? Json(slEvaluation.frameIndex) : Json(nullptr) },
                { "feature", slEvaluation.observed ? Json(slEvaluation.feature) : Json(nullptr) },
                { "scope_depth", slEvaluation.depth },
                { "viewport_status", FSRD::SlEvaluationProvenance::ViewportStatusName(slEvaluation.viewportStatus) },
                { "viewport", slEvaluation.observed && slEvaluation.viewportStatus ==
                    FSRD::SlEvaluationProvenance::ViewportStatus::Observed ? Json(slEvaluation.viewport) : Json(nullptr) },
                { "command_buffer_address", slEvaluation.observed ?
                    Json(std::format("{:x}", slEvaluation.commandBuffer)) : Json(nullptr) },
                { "raw_command_buffer_equals_ngx_list", slEvaluation.observed ?
                    Json(slEvaluation.commandBuffer == uintptr_t(list)) : Json(nullptr) },
                { "command_buffer_canonical_identity", "not_established" },
                { "fog_frame_association", "not_established" },
                { "gpu_execution_order", "not_established" }
            };
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
                    { "sl_evaluation_scope", slProvenance },
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
                if (trace->candidates.size() < MaxCaptureCandidates && recordingKnown && color &&
                    record["color"]["matches_fog_resource"].get<bool>() && renderWidth && renderHeight)
                {
                    const unsigned candidateIndex = UINT(trace->candidates.size()) + 1;
                    Json metadata = {
                        { "schema", "optiscaler.fsr_rr.fog_capture_candidate.v1" },
                        { "status", "unvalidated_candidate" }, { "session_key", trace->sessionKey },
                        { "process_id", GetCurrentProcessId() }, { "fog_scope_serial", trace->fog["scope_serial"] },
                        { "candidate_index", candidateIndex }, { "rr_feature", featureId }, { "rr_frame", frameIndex },
                        { "sl_evaluation_scope", slProvenance },
                        { "fog_origin", trace->fog }, { "rr_command_list_identity", record["command_list_identity"] },
                        { "rr_recording_generation", generation }, { "rr_endpoint_ordinal", ordinal },
                        { "owned_resource_identity", record["color"]["canonical_identity"] },
                        { "color_address", record["color"]["address"] }, { "render_extent", { renderWidth, renderHeight } },
                        { "local_recording_relation", relation }, { "provenance_file", trace->sidecarRelative },
                        { "owned_identities_retained", true },
                        { "acceptance", "requires completed submission evidence and independent native pixel comparison; not a frame proof" }
                    };
                    auto candidate = std::make_shared<NgxCaptureCandidate>();
                    candidate->metadata = metadata.dump();
                    candidate->index = candidateIndex;
                    candidate->ownership = trace;
                    trace->candidates.push_back({ identity, generation, std::move(metadata) });
                    result = std::move(candidate); // Consumed now, never requeued if this Evaluate fails.
                    record["capture_candidate_index"] = candidateIndex;
                }
                if (trace->count == MaxNgxEndpoints)
                {
                    trace->endpointsClosed = true;
                    endpointActive.store(false, std::memory_order_release);
                    data.endpoint.reset(); // Local shared owners outlive the lock and final log.
                }
            }
            LOG_INFO("[FSRRR fog endpoint] NGX {}", record.dump());
            FinalizeSubmissionTrace(trace);
        }
        catch (const std::exception& error)
        {
            endpointActive.store(false, std::memory_order_release);
            {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                trace = std::move(data.endpoint);
                if (trace) trace->failure = error.what();
            }
            LOG_WARN("[FSRRR fog endpoint] observation window stopped; association incomplete: {}", error.what());
            FinalizeSubmissionTrace(trace);
        }
    });
    return result;
}

void CandidateCaptureResult(const CaptureCandidate& candidate, bool started) noexcept
{
    if (!candidate || !candidate->ownership)
        return;
    Metadata([&] {
        const auto trace = std::static_pointer_cast<EndpointTrace>(candidate->ownership);
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            if (!candidate->index || candidate->index > trace->candidates.size() || trace->finalized)
                return;
            auto& state = trace->candidates[candidate->index - 1];
            if (state.reported)
                return; // An exact-Evaluate result is immutable once reported.
            state.reported = true;
            state.started = started;
        }
        FinalizeSubmissionTrace(trace);
    });
}

struct SubmissionObservation
{
    std::shared_ptr<EndpointTrace> trace;
    std::shared_ptr<SubmissionData> record;
};

SubmissionObservationToken PreparingSubmission(ID3D12CommandQueue* queue, UINT count,
                                                ID3D12CommandList* const* lists) noexcept
{
    if (!submissionActive.load(std::memory_order_acquire))
        return {};
    const uint64_t entry = submissionSerial.fetch_add(1) + 1;
    SubmissionObservationToken result;
    Metadata([&] {
        std::shared_ptr<EndpointTrace> trace;
        try
        {
            auto record = std::make_shared<SubmissionData>();
            if (!queue || FAILED(queue->QueryInterface(IID_PPV_ARGS(&record->queue))))
                throw std::runtime_error("submission queue identity unavailable");
            record->entry = entry;
            record->lists = Json::array();
            bool matched = false;
            {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                trace = data.submissionTrace;
                if (!trace || trace->finalized)
                    return;
                if (++trace->observedSubmissions > MaxObservedSubmissions || !lists || !count ||
                    count > MaxListsPerSubmission || GetTickCount64() - trace->startedAt > SubmissionWindowMs)
                    throw std::runtime_error("bounded submission observation window exceeded");
                for (UINT i = 0; i < count; ++i)
                {
                    ComPtr<IUnknown> identity;
                    if (!lists[i] || FAILED(lists[i]->QueryInterface(IID_PPV_ARGS(&identity))))
                        throw std::runtime_error("submitted list identity unavailable");
                    const auto found = data.lists.find(identity.Get());
                    const bool known = captureTrackingValid.load() && found != data.lists.end();
                    const uint64_t generation = known ? found->second.generation : 0;
                    Json roles = Json::array();
                    if (known && identity.Get() == trace->list.Get() && generation == trace->generation)
                        roles.push_back("fog");
                    for (size_t c = 0; c < trace->candidates.size(); ++c)
                        if (known && identity.Get() == trace->candidates[c].list.Get() &&
                            generation == trace->candidates[c].generation)
                            roles.push_back(std::format("candidate-{}", c + 1));
                    matched |= !roles.empty();
                    record->lists.push_back({ { "array_index", i },
                        { "command_list_identity", std::format("{:x}", uintptr_t(identity.Get())) },
                        { "recording_known", known },
                        { "recording_generation", known ? Json(generation) : Json(nullptr) }, { "roles", roles } });
                }
                if (matched)
                {
                    if (trace->submissions.size() >= 8)
                        throw std::runtime_error("matched recording submission budget exceeded");
                    trace->submissions.push_back(record);
                    result = std::make_shared<SubmissionObservation>(SubmissionObservation { trace, record });
                }
            }
        }
        catch (const std::exception& error)
        {
            if (trace)
            {
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                trace->failure = error.what();
            }
            FinalizeSubmissionTrace(trace);
        }
    });
    return result;
}

void SubmittedSubmission(const SubmissionObservationToken& observation) noexcept
{
    if (!observation)
        return;
    const uint64_t exit = submissionSerial.fetch_add(1) + 1;
    Metadata([&] {
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            if (!observation->record->exit)
                observation->record->exit = exit;
        }
        FinalizeSubmissionTrace(observation->trace);
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
            const auto image = entry - FogNodeRva;
            authenticatedImage.store(image);
            const bool captures = Config::Instance()->FfxDenoiserCyberpunkFogCapture.value_or_default();
            if (captures && std::all_of(std::begin(ProducerCode), std::end(ProducerCode),
                                       [&](const auto& code) { return MatchLiveCode(image, code); }))
                originalGBufferInitializer = reinterpret_cast<GBufferInitializer>(image + GBufferInitializerRva);
            if (captures && std::all_of(std::begin(LightingCode), std::end(LightingCode),
                                       [&](const auto& code) { return MatchLiveCode(image, code); }))
            {
                originalLightingNode = reinterpret_cast<FogNode>(image + LightingNodeRva);
                originalFullscreenHelper = reinterpret_cast<FullscreenHelper>(image + FullscreenHelperRva);
            }
            originalFogNode = reinterpret_cast<FogNode>(entry);
            LONG error = DetourTransactionBegin();
            if (error == NO_ERROR)
            {
                error = DetourUpdateThread(GetCurrentThread());
                if (error == NO_ERROR)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalFogNode), HookFogNode);
                if (error == NO_ERROR && originalGBufferInitializer)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalGBufferInitializer), HookGBufferInitializer);
                if (error == NO_ERROR && originalLightingNode)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalLightingNode), HookLightingNode);
                if (error == NO_ERROR && originalFullscreenHelper)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalFullscreenHelper), HookFullscreenHelper);
                if (error == NO_ERROR)
                    error = DetourTransactionCommit();
                else
                    DetourTransactionAbort();
            }
            if (error != NO_ERROR)
            {
                originalFogNode = nullptr;
                originalGBufferInitializer = nullptr;
                originalLightingNode = nullptr;
                originalFullscreenHelper = nullptr;
                authenticatedImage.store(0);
                LOG_WARN("[FSRRR fog probe] engine hook failed: {}", error);
                return;
            }
            captureEnabled.store(captures);
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
        originalCreateCompute = reinterpret_cast<CreateCompute>(table[11]);
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
        if (error == NO_ERROR && originalCreateCompute)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalCreateCompute), HookCreateCompute);
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
        originalCreateCompute = nullptr;
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
        originalClearDsv = reinterpret_cast<ClearDsv>(table[47]);
        originalClearRtv = reinterpret_cast<ClearRtv>(table[48]);
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
        if (error == NO_ERROR && originalClearRtv)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalClearRtv), HookClearRtv);
        if (error == NO_ERROR && originalClearDsv)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalClearDsv), HookClearDsv);
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
        originalClearRtv = nullptr;
        originalClearDsv = nullptr;
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

#include "pch.h"
#include "FSRDCyberpunkFogProbe.h"

#include <Util.h>
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
#include <string_view>
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
};
struct Registry
{
    std::mutex mutex;
    CryptoApi crypto;
    std::vector<TaggedPso> tagged;
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
        if (!lock || arm.load() >= MaxRearms)
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

void WINAPI HookDraw(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT start, UINT firstInstance)
{
    LogDraw(list, false, count, instances, start, 0, firstInstance);
    originalDraw(list, count, instances, start, firstInstance);
}
void WINAPI HookDrawIndexed(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT start,
                            INT baseVertex, UINT firstInstance)
{
    LogDraw(list, true, count, instances, start, baseVertex, firstInstance);
    originalDrawIndexed(list, count, instances, start, baseVertex, firstInstance);
}

void RecordPso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3D12PipelineState* pso, const char* path)
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
        data.tagged.push_back({ pso, identity.name });
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
                RecordPso(parsed.PipelineStream.GraphicsDescV0(), pso.Get(), "CreatePipelineState");
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
            active.store(true);
            LOG_INFO("[FSRRR fog probe] enabled metadata-only; authenticated Cyberpunk 2.31 file 3.0.80.51928 SHA256={} RVA={:x}; first {} draws per arm; pipeline-library loads not authenticated",
                     ExeSha256, FogNodeRva, MaxDrawLogs);
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
        if (error == NO_ERROR)
            error = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    if (error != NO_ERROR)
    {
        originalCreateGraphics = nullptr;
        originalCreateStream = nullptr;
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
    }
    LOG_INFO("[FSRRR fog probe] command-list metadata hooks result={}", error);
}
} // namespace FSRDCyberpunkFogProbe

#include "pch.h"
#include "FSRDCyberpunkFogProbe.h"
#include "FSRDFogLayerCapture.h"
#include "FSRDCyberpunkEarlyGuides.h"
#include "FSRDCyberpunkEngineAccess.h"
#include "FSRDCyberpunkGuideMatrix.h"
#include "FSRDCyberpunkGuidePass.h"
#include "FSRDCyberpunkLightingShaders.h"
#include "FSRDCyberpunkExposurePass.h"
#include "FSRDCyberpunkExposureSource.h"
#include "FSRDCyberpunkLightingSource.h"
#include "FSRDCyberpunkRayConstants.h"
#include "FSRDCyberpunkRayBindings.h"
#include "FSRDCyberpunkRayAccess.h"
#include "FSRDCyberpunkFogDepth.h"
#include "FSRDCyberpunkFogDepthCopy.h"
#include "FSRDCyberpunkFogDenoiseAccess.h"
#include "FSRDCyberpunkFogRgbWrite.h"
#include "FSRDPreFogSession.h"
#include "FSRDPrivateRayCopy.h"
#include "FSRDCyberpunkResetCamera.h"
#include "FSRDCyberpunkLightingConstants.h"
#include "FSRDCyberpunkPrivateResetSource.h"
#include "FSRDCyberpunkPrivateResetPolicy.h"
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include "FSRDCyberpunkTemporalCamera.h"
#include "FSRDPrivateDenoise.h"

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
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
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
// Original20cc64 calls this with EDX=2 before the admitted native Draw. It
// maps2 to TRIANGLELIST4 and sets/caches native IA topology at engine+0x628.
// There are no directly branched external cold chunks in this exact body.
constexpr FSRD::CyberpunkEngineAccess::CodeRange FogTopologyCode {
    0x1f6fbc, 0x1a7, "2c52a561dbce82c04eed3c3b0cfa4ee525ae3aee61d430529b9ff1a6b579b275" };
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
struct PrivateResetPacket;
struct TemporalCharge;
struct RayCopyBundle
{
    FSRD::CyberpunkRayAccess::Input input;
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> listIdentity;
    FSRD::PrivateRayCopy::Textures sources;
    std::shared_ptr<FSRD::PrivateRayCopy::Work> work;
    uintptr_t frameSourceObject = 0, rayOwner = 0;
    UINT width = 0, height = 0;
    unsigned dispatchOrdinal = 0, completedDispatches = 0;
    bool originalReturned = false, cleanupEntered = false, cleanupConsumed = false, invalidated = false, recorded = false;
    Json provenance;
    bool privateReset = false;
    PrivateResetPacket* packet = nullptr; // Immutable scoped route, not an owning back-reference.
    std::shared_ptr<TemporalCharge> charge;
};
struct Registry
{
    std::mutex mutex;
    CryptoApi crypto;
    FSRD::CyberpunkLightingShaders::Registry lightingShaders;
    std::vector<TaggedPso> tagged;
    std::vector<RtvHeap> heaps;
    std::vector<CpuSrvHeap> cpuSrvHeaps;
    UINT64 cpuSrvSlots = 0, cpuSrvBytes = 0;
    std::vector<std::byte> guideShader;
    std::shared_ptr<EarlyProducer> earlyProducer;
    // Bounded immutable CPU-upload snapshots, never live GPU/frame authority.
    std::vector<FSRD::CyberpunkRayConstants::Receipt> rayConstants;
    // CPU descriptor correspondence at original DispatchRays only. These are
    // not retained resource leases, GPU payloads, or a later-lighting frame join.
    std::vector<Json> rayDispatches;
    std::shared_ptr<RayCopyBundle> rayCopy; // One immutable, privately owned same-list snapshot bundle.
    Json rayCopyStatus;
    Json lightingCaptureSource; // One completed-recording receipt, never cross-list GPU ordering.
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
std::atomic<bool> rayBindingsAuthenticated { false };
std::atomic<bool> fogDepthAuthenticated { false };
std::atomic<bool> rayCopyAttempted { false };
std::atomic<ULONGLONG> lightingRequestedAt { 0 };
std::atomic<bool> endpointActive { false };
std::atomic<bool> submissionActive { false };
std::atomic<uint64_t> submissionSerial { 0 };
std::atomic<unsigned> drawLogs { 0 }, emptyLogs { 0 };
std::atomic<unsigned> arm { 0 };
std::atomic<uint64_t> scopes { 0 };
std::mutex hookMutex;
std::mutex captureRequestMutex; // Serializes ALL marker admission, not native recording.

namespace ResetPolicy = FSRD::CyberpunkPrivateResetPolicy;
namespace ResetSource = FSRD::CyberpunkPrivateResetSource;
namespace WindowPolicy = FSRD::CyberpunkTemporalWindowPolicy;
namespace TemporalCamera = FSRD::CyberpunkTemporalCamera;
struct TemporalWindow;
struct PacketPolicy
{
    explicit PacketPolicy(uintptr_t identity) : local(identity) {}
    ResetPolicy::Policy local;
    TemporalWindow* window = nullptr; // Controller is never owned by a GPU ticket.
    WindowPolicy::FrameKey key {};
    bool DeclareProducer(ResetPolicy::Recording recording) noexcept;
    bool SealProducer(const ResetPolicy::ProducerSeal& seal) noexcept;
    bool EmbedConsumer(ResetPolicy::Recording recording, uint64_t first, bool owners) noexcept;
    bool SealConsumer(ResetPolicy::Recording recording, uint64_t last, bool success, bool restored) noexcept;
    bool Failed() const noexcept;
    void Fail() noexcept;
    ResetPolicy::Decision BeforeExecute(ResetPolicy::Queue queue, std::span<const ResetPolicy::Recording> lists) noexcept
    { return local.BeforeExecute(queue, lists); } // One-shot path only; window demux never calls this.
    bool AfterExecute(uint64_t token) noexcept { return local.AfterExecute(token); }
};
struct TemporalBudget
{
    std::atomic<UINT64> bytes { 0 };
    static constexpr UINT64 Limit = 512ull * 1024 * 1024;
};
// Leaf budget ownership follows the same fence-retained owners as the resources.
// It contains no packet, Work, converter, ticket, or controller back-reference.
struct TemporalCharge
{
    explicit TemporalCharge(std::shared_ptr<TemporalBudget> value) : budget(std::move(value)) {}
    std::shared_ptr<TemporalBudget> budget;
    UINT64 bytes = 0;
    std::mutex mutex;
    void Add(UINT64 amount)
    {
        std::lock_guard lock(mutex);
        auto old = budget->bytes.load();
        do {
            if (!amount || amount > TemporalBudget::Limit - old)
                throw std::runtime_error("temporal retained host texture budget exhausted");
        } while (!budget->bytes.compare_exchange_weak(old, old + amount));
        bytes += amount;
    }
    ~TemporalCharge() { budget->bytes.fetch_sub(bytes); }
};
struct PrivateResetPacket
{
    explicit PrivateResetPacket(uintptr_t identity) : policy(identity) {}
    std::mutex mutex;
    PacketPolicy policy;
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> deviceIdentity, producerIdentity, consumerIdentity, queueIdentity;
    std::shared_ptr<FSRD::CyberpunkGuidePass::Targets> guides;
    std::shared_ptr<FSRD::PrivateRayCopy::Targets> rays;
    // Not retained by a submission ticket: the Work/converter/ticket graph must
    // remain acyclic. This one explicit packet survives process teardown.
    std::shared_ptr<FSRD::PrivateDenoise::Work> denoise;
    std::optional<ResetSource::RawSource> source;
    FSRD::DenoiserSettings settings {};
    uint64_t provider = 0, rayTerminal = 0, guideBegin = 0;
    UINT width = 0, height = 0;
    float delta = 0;
    bool rayClaimed = false, guideClaimed = false, fogClaimed = false;
    bool sceneResetOnce = false; // Immutable after publication; requires fixed late-SR-only session.
    ResetPolicy::Recording producer {};
    ResetPolicy::Recording consumer {};
    TemporalWindow* temporal = nullptr;
    WindowPolicy::FrameKey temporalKey {};
    std::shared_ptr<TemporalCharge> charge;
    std::shared_ptr<RayCopyBundle> rayCopy;
    std::shared_ptr<FSRDSubmission::Ticket> finalTicket;
    std::optional<ResetSource::TemporalSource> timedSource;
    double fogTimestamp = 0, previousFogTimestamp = 0;
    bool sceneRecorded = false, consumerSealed = false, returned = false, retired = false;
};
struct TemporalTargets
{
    std::shared_ptr<FSRD::CyberpunkGuidePass::Targets> guides;
    std::shared_ptr<FSRD::PrivateRayCopy::Targets> rays;
    std::shared_ptr<TemporalCharge> charge;
};
struct TemporalWindow
{
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> deviceIdentity, queueIdentity, warmupList;
    std::shared_ptr<FSRD::PrivateDenoise::Session> session;
    std::shared_ptr<TemporalBudget> budget = std::make_shared<TemporalBudget>();
    std::unique_ptr<WindowPolicy::Window> policy;
    std::array<std::unique_ptr<PrivateResetPacket>, 32> frames;
    std::array<std::optional<TemporalTargets>, 2> freeTargets;
    std::optional<ResetSource::RawSource> lastFog;
    std::optional<TemporalCamera::PreviousFrame> previous;
    ResetPolicy::Recording warmupRecording {};
    WindowPolicy::Receipt pendingWarmup {};
    std::array<WindowPolicy::Receipt, WindowPolicy::Window::MaxPendingCalls> pendingCalls {};
    FSRD::DenoiserSettings settings {};
    uint64_t epoch = 0, provider = 0, warmupSerial = 0;
    uint32_t warmupSkippedThrough = 0;
    UINT width = 0, height = 0;
    double lastFogTimestamp = 0;
    bool warmupReturned = false, allocating = false, finalCaptureQueued = false;
    std::atomic<bool> stopped { false };
    std::string failure;
    std::array<Json, 32> ledger;
    std::string ledgerRelative;
    bool ledgerSaved = false;
    uint32_t returnEvidencePending = 0;
    bool ledgerEvidenceLost = false;
};
std::atomic<TemporalWindow*> temporalWindow { nullptr }; // One exclusive, explicit bounded window/process.
std::atomic<uint64_t> nextTemporalEpoch { 0 };

bool PacketPolicy::DeclareProducer(ResetPolicy::Recording recording) noexcept
{
    if (!window) return local.DeclareProducer(recording);
    std::lock_guard lock(window->mutex);
    return window->policy && window->policy->DeclareProducer(key, recording);
}
bool PacketPolicy::SealProducer(const ResetPolicy::ProducerSeal& seal) noexcept
{
    if (!window) return local.SealProducer(seal);
    std::lock_guard lock(window->mutex);
    return window->policy && window->policy->SealProducer(key, seal);
}
bool PacketPolicy::EmbedConsumer(ResetPolicy::Recording recording, uint64_t first, bool owners) noexcept
{
    if (!window) return local.EmbedConsumer(recording, first, owners);
    std::lock_guard lock(window->mutex);
    return !window->stopped && window->policy && window->policy->EmbedConsumer(key, recording, first, owners);
}
bool PacketPolicy::SealConsumer(ResetPolicy::Recording recording, uint64_t last, bool success, bool restored) noexcept
{
    if (!window) return local.SealConsumer(recording, last, success, restored);
    std::lock_guard lock(window->mutex);
    return window->policy && window->policy->SealConsumer(key, recording, last, success, restored);
}
bool PacketPolicy::Failed() const noexcept
{
    if (!window) return local.Failed();
    std::lock_guard lock(window->mutex);
    return !window->policy || window->policy->FrameFailed(key);
}
void PacketPolicy::Fail() noexcept
{
    if (!window) { local.Fail(); return; }
    bool embedded = false;
    {
        std::lock_guard lock(window->mutex);
        embedded = window->policy && window->policy->ConsumerEmbedded(key);
        if (window->policy) window->policy->FailFrame(key);
        window->stopped = true;
    }
    // A failed, still-private look-ahead producer must not cancel the earlier
    // valid consumer acknowledgement. An embedded failed consumer is different:
    // its history cannot be trusted and its original submission is vetoed.
    if (embedded && window->session) window->session->Stop();
}
// Published only after CPU allocation/provider initialization has completed on
// the late RR caller. Exactly one packet/process; never replace an embedded read.
std::atomic<PrivateResetPacket*> privateResetPacket { nullptr };
struct RgbIdentityPacket
{
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> deviceIdentity;
    UINT width = 0, height = 0;
};
// Dedicated one-shot identity control. Never replaces/joins a RESET packet or
// changes the late RR route; no early guide/ray producer is requested.
std::atomic<RgbIdentityPacket*> rgbIdentityPacket { nullptr };
// Shared publication latch for both exclusive diagnostic modes. It also keeps
// first native consumers out while either request transaction is incomplete.
std::atomic<bool> privateResetArming { false };

[[noreturn]] void PrivateResetFatal() noexcept
{
    earlyFatalRecording.store(true);
    try { LOG_ERROR("[FSRRR pre-Fog diagnostic] FATAL unsafe recording/dependency; refusing further native work in authenticated Cyberpunk"); }
    catch (...) {}
    // This function is reachable only through a diagnostic armed under the exact
    // executable authentication. Never terminates a launcher or another process.
    TerminateProcess(GetCurrentProcess(), 0xf51d0002u);
    RaiseFailFastException(nullptr, nullptr, 0);
    std::terminate();
}

using FogNode = void(__fastcall*)(void* node, void* context);
FogNode originalFogNode = nullptr;
FogNode originalLightingNode = nullptr;
FogNode originalRayNode = nullptr;
using RayCleanup = void(__fastcall*)(void*, uint8_t);
RayCleanup originalRayCleanup = nullptr;
using FullscreenHelper = void(__fastcall*)(void*, uint32_t, uint8_t);
FullscreenHelper originalFullscreenHelper = nullptr;
using BindTextures = void(__fastcall*)(uint32_t, uint32_t, const uint32_t*, uint8_t);
BindTextures originalBindTextures = nullptr;
using BindUavs = void(__fastcall*)(uint32_t, uint32_t, const uint32_t*);
BindUavs originalBindUavs = nullptr;
using UploadLightingConstants = void(__fastcall*)(uint32_t, const void*);
UploadLightingConstants originalUploadLightingConstants = nullptr;
UploadLightingConstants originalUploadRayConstants = nullptr;
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
using DispatchRays = rewrite_signature<decltype(&ID3D12GraphicsCommandList4::DispatchRays)>::type;
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
DispatchRays originalDispatchRays = nullptr;
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
    bool fogHelper = false, depthBindObserved = false;
    unsigned depthBindCalls = 0;
    uint32_t depthHandle = 0;
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
    bool shaderArgumentObserved = false;
    uint32_t shaderArgument = 0;
    unsigned finalDraws = 0;
    unsigned t8BindCalls = 0;
    bool t8BindObserved = false;
    uint32_t t8Handle = 0, t8BindCount = 0;
    uintptr_t t8BindCaller = 0;
    FSRD::CyberpunkLightingConstants::Receipt lightingConstants;
};
thread_local LightingScope* lightingScope = nullptr;
struct RayBindReceipt
{
    FSRD::CyberpunkRayConstants::Scope scope;
    uintptr_t callerRva = 0;
    uint32_t handle = 0, writes = 0;
    bool expectedSeen = false, valid = false;
};
constexpr std::array<uint32_t, 3> RayBindingRegisters { 4, 0, 8 };
constexpr std::array<uintptr_t, 3> RayBindingReturnRvas { 0xc6ae45, 0xc6bb56, 0xc6bb44 };
constexpr unsigned MaxRayDispatches = 8;
struct RayScope
{
    RayScope* previous = nullptr;
    uint64_t serial = 0;
    void* context = nullptr;
    FSRD::CyberpunkRayConstants::Receipt receipt {};
    std::array<RayBindReceipt, 3> bindings {}; // Original t4 SRV, u0 UAV, u8 UAV.
    unsigned dispatches = 0;
    unsigned nativeDispatchDepth = 0;
    std::shared_ptr<RayCopyBundle> pendingCopy {}; // Only this original invocation may record it before end-use.
};
thread_local RayScope* rayScope = nullptr;

void InvalidateRayBinding(RayBindReceipt& receipt) noexcept
{
    receipt.valid = false;
    if (receipt.writes != UINT32_MAX) ++receipt.writes;
    else receipt.expectedSeen = true; // Saturation cannot resurrect a pending receipt.
}

bool RayBindingsArmed() noexcept
{
    return rayScope && !inMetadata && rayBindingsAuthenticated.load() &&
        captureTrackingValid.load() && lightingRequested.load() && !lightingAttempted.load();
}

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

uint64_t PrivateResetPoint(IUnknown* identity, uint64_t generation)
{
    auto& data = Data();
    std::lock_guard lock(data.mutex);
    const auto found = data.lists.find(identity);
    if (!captureTrackingValid.load() || found == data.lists.end() || !found->second.known ||
        found->second.generation != generation || found->second.predicated || found->second.renderPass ||
        found->second.queryCount)
        throw std::runtime_error("private RESET recording generation/state unavailable");
    return ++found->second.endpointOrdinal;
}

// The same packet can be encountered first by any CPU recording worker. Key
// equality is necessary, but the independent pre-Execute policy proves ordering.
void ClaimPrivateReset(PrivateResetPacket& packet, const Json& metadata, ID3D12Device* device,
                       bool& role)
{
    ResetSource::RawSource source = packet.temporal ? ResetSource::ParseRawTemporal(metadata).current :
        static_cast<ResetSource::RawSource>(ResetSource::Parse(metadata, packet.delta));
    ComPtr<IUnknown> identity;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) ||
        identity.Get() != packet.deviceIdentity.Get() || source.width != packet.width || source.height != packet.height)
        throw std::runtime_error("private RESET active device/extent differs from allocated targets");
    std::lock_guard lock(packet.mutex);
    if (packet.policy.Failed() || role || (packet.source && !packet.source->SameFrame(source)))
    {
        packet.policy.Fail();
        throw std::runtime_error("private RESET repeated role or exact current-frame/camera mismatch");
    }
    if (!packet.source) packet.source = std::move(source);
    role = true;
}

void StopTemporalWindow(TemporalWindow& window, const char* reason) noexcept
{
    try
    {
        bool first = false;
        {
            std::lock_guard lock(window.mutex);
            first = !window.stopped;
            window.stopped = true;
            if (window.policy) window.policy->Stop(); // Existing obligations still drain.
            if (window.failure.empty()) window.failure = reason;
        }
        if (first) LOG_WARN("[FSRRR temporal] window stopped: {}; late SR remains fixed, no fallback or reset retry", reason);
    }
    catch (...) { window.stopped = true; }
}

bool TemporalRecordingRequested() noexcept
{
    // Keep authentic original-use receipts active while a stopped window drains
    // already embedded obligations. ClaimRole decides which exact frame may use them.
    return temporalWindow.load(std::memory_order_acquire) != nullptr;
}

PrivateResetPacket* SelectTemporalFrame(TemporalWindow& window, const Json& metadata,
                                        ID3D12Device* device, WindowPolicy::Role role)
{
    const auto source = ResetSource::ParseRawTemporal(metadata);
    ComPtr<IUnknown> identity;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) ||
        identity.Get() != window.deviceIdentity.Get() || source.current.width != window.width ||
        source.current.height != window.height)
    { StopTemporalWindow(window, "current role device or extent changed"); return nullptr; }
    std::lock_guard lock(window.mutex);
    if (!window.warmupReturned || !window.queueIdentity || !window.lastFog)
    {
        // An actual producer may have already passed before the warm-up queue
        // returned. Never later start that same frame's consumer and hope that
        // a missed original-use producer will run again. This is a refusal
        // watermark only, not a fabricated frame or resource association.
        window.warmupSkippedThrough = std::max(window.warmupSkippedThrough, source.current.frame);
        return nullptr;
    }
    if (source.current.view != window.lastFog->view || source.current.object != window.lastFog->object)
    {
        window.stopped = true;
        if (window.policy) window.policy->Stop();
        return nullptr;
    }
    if (!window.policy)
    {
        if (source.current.frame <= window.warmupSkippedThrough) return nullptr;
        if (window.stopped || source.current.frame <= window.lastFog->frame) return nullptr;
        if (window.lastFog->frame == UINT32_MAX || source.current.frame != window.lastFog->frame + 1)
        { window.stopped = true; return nullptr; }
        window.policy = std::make_unique<WindowPolicy::Window>(window.epoch,
            ResetPolicy::Queue { uintptr_t(window.queueIdentity.Get()), uintptr_t(window.deviceIdentity.Get()), true },
            source.current.frame);
    }
    if (window.policy->Complete()) return nullptr;
    const auto key = window.policy->ClaimRole(source.current.frame, role);
    if (!key.Valid()) { window.stopped = true; return nullptr; }
    if (key.index && source.nativeResetRequested)
    { window.policy->Stop(); window.stopped = true; return nullptr; }
    auto& frame = window.frames[key.index];
    if (!frame)
    {
        auto free = std::find_if(window.freeTargets.begin(), window.freeTargets.end(),
                                  [](const auto& item) { return item.has_value(); });
        if (free == window.freeTargets.end())
        { window.policy->Stop(); window.stopped = true; return nullptr; }
        if (key.CaptureFinal())
        {
            // Only this exact final frame can use the two existing one-shot disk
            // slots. Global Wants flags alone never route a temporal callback.
            if (!FSRDFogLayerCapture::RequestEarlyGuides())
            { window.policy->Stop(); window.stopped = true; return nullptr; }
            if (!FSRDFogLayerCapture::Request())
            {
                FSRDFogLayerCapture::CancelEarlyGuideRequest();
                window.policy->Stop(); window.stopped = true; return nullptr;
            }
            window.finalCaptureQueued = true;
        }
        auto created = std::make_unique<PrivateResetPacket>(uintptr_t(window.deviceIdentity.Get()));
        created->device = window.device; created->deviceIdentity = window.deviceIdentity;
        created->width = window.width; created->height = window.height;
        created->provider = window.provider; created->settings = window.settings;
        created->temporal = &window; created->temporalKey = key;
        created->policy.window = &window; created->policy.key = key;
        created->guides = std::move((*free)->guides); created->rays = std::move((*free)->rays);
        created->charge = std::move((*free)->charge);
        free->reset(); // Moved-from: no COM-backed owner destruction under lock.
        frame = std::move(created);
    }
    return frame.get();
}

void FailPacket(PrivateResetPacket* packet) noexcept
{ if (packet) { std::lock_guard lock(packet->mutex); packet->policy.Fail(); } }

void FailPrivateReset() noexcept
{
    if (auto* packet = privateResetPacket.load(std::memory_order_acquire))
    {
        std::lock_guard lock(packet->mutex);
        packet->policy.Fail(); // Never clear a consumer obligation on an exception.
    }
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
        std::unique_lock lock(captureRequestMutex, std::try_to_lock);
        if (!lock || privateResetArming.load(std::memory_order_acquire) ||
            rgbIdentityPacket.load(std::memory_order_acquire) || TemporalRecordingRequested())
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
    // Nested original callbacks can reuse TLS upload/binding descriptor slots.
    // Never let a parent's old receipt survive that intervening original work.
    for (auto* parent = lightingScope; parent; parent = parent->previous)
    {
        parent->t8BindObserved = false;
        FSRD::CyberpunkLightingConstants::Invalidate(parent->lightingConstants);
    }
    LightingScope current { lightingScope, scopes.fetch_add(1) + 1, node, context };
    lightingScope = &current;
    struct Restore { LightingScope* previous; ~Restore() { lightingScope = previous; } } restore { current.previous };
    originalLightingNode(node, context); // Exactly one original callback, including ordinary refusal paths.
}

void __fastcall HookRayNode(void* node, void* context)
{
    for (auto* parent = rayScope; parent; parent = parent->previous)
    {
        FSRD::CyberpunkRayConstants::Invalidate(parent->receipt);
        for (auto& binding : parent->bindings) InvalidateRayBinding(binding);
        if (parent->pendingCopy) parent->pendingCopy->invalidated = true;
    }
    RayScope current { rayScope, scopes.fetch_add(1) + 1, context };
    rayScope = &current;
    struct Restore { RayScope* previous; ~Restore() { rayScope = previous; } } restore { current.previous };
    originalRayNode(node, context); // Original exactly once, never caught/replayed.
    if (current.pendingCopy)
        Metadata([&] {
            current.pendingCopy->provenance["status"] = "refused_before_state_requests";
            current.pendingCopy->provenance["reason"] = "original ray node returned without the admitted common cleanup endpoint";
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            data.rayCopyStatus = current.pendingCopy->provenance;
        });
}

FSRD::CyberpunkRayConstants::Scope CurrentRayConstantScope()
{
    FSRD::CyberpunkRayConstants::Scope result;
    uint8_t initialized = 0;
    if (!rayScope || !ReadEarly(uintptr_t(__readgsqword(0x58)), result.tls) ||
        !ReadEarlyAt(result.tls, 0x14, initialized) || !initialized ||
        !ReadEarlyAt(result.tls, 0x188, result.engine) ||
        !ReadEarlyAt(result.engine, 0x30, result.list))
        throw std::runtime_error("current ray upload TLS/list unavailable");
    result.serial = rayScope->serial;
    result.graphContext = uintptr_t(rayScope->context);
    uintptr_t object = 0, vtable = 0, getter = 0;
    constexpr std::array<uint8_t, 5> GetterBytes { 0x48, 0x8d, 0x41, 0x10, 0xc3 };
    std::array<uint8_t, 5> bytes {};
    uint32_t repeatedFrame = 0;
    if (!ReadEarlyAt(result.graphContext, 0x18, result.view) ||
        !ReadEarlyAt(result.graphContext, 0, object) || !ReadEarlyAt(object, 0, vtable) ||
        !ReadEarlyAt(vtable, 0x20, getter) || getter != authenticatedImage.load() + 0x18ec810 ||
        !ReadEarly(getter, bytes) || bytes != GetterBytes ||
        !ReadEarlyAt(object, 0x1b0, result.frameSource) ||
        !ReadEarlyAt(object, 0x1b0, repeatedFrame) || repeatedFrame != result.frameSource)
        throw std::runtime_error("current ray CPU frame-source route unavailable");
    auto identity = ListIdentity(reinterpret_cast<ID3D12GraphicsCommandList*>(result.list));
    auto& data = Data();
    std::lock_guard lock(data.mutex);
    const auto found = data.lists.find(identity.Get());
    if (found == data.lists.end() || !found->second.known)
        throw std::runtime_error("current ray list generation unavailable");
    result.recordingGeneration = found->second.generation;
    return result;
}

bool BeginRayBind(RayScope& current, size_t index, uintptr_t caller, uint32_t first, uint32_t count,
                  const uint32_t* handles, RayBindReceipt& pending)
{
    pending = {};
    if (index >= current.bindings.size()) return false;
    const auto reg = RayBindingRegisters[index];
    if (first > reg || uint64_t(first) + count <= reg) return false;
    auto& receipt = current.bindings[index];
    InvalidateRayBinding(receipt); // Any covering write, including unsupported/null binds.
    const auto image = authenticatedImage.load();
    if (receipt.expectedSeen || !image || first != reg || count != 1 ||
        caller != image + RayBindingReturnRvas[index]) return false;
    receipt.expectedSeen = true;
    uint32_t handle = 0;
    if (!ReadEarly(uintptr_t(handles), handle) || !handle || handle > FSRD::CyberpunkRayBindings::TextureSlots)
        return false;
    receipt.scope = CurrentRayConstantScope();
    receipt.handle = handle;
    receipt.callerRva = RayBindingReturnRvas[index];
    pending = receipt;
    return true;
}

void CompleteRayBind(RayScope* current, size_t index, const RayBindReceipt& pending, const uint32_t* handles)
{
    if (rayScope != current || !current || index >= current->bindings.size()) return;
    auto& receipt = current->bindings[index];
    uint32_t repeated = 0;
    if (receipt.writes != pending.writes || receipt.writes == UINT32_MAX ||
        !receipt.expectedSeen || !ReadEarly(uintptr_t(handles), repeated) || repeated != pending.handle ||
        CurrentRayConstantScope() != pending.scope) return;
    receipt.valid = true;
}

void __fastcall HookUploadRayConstants(uint32_t bytes, const void* source)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* current = rayScope;
    bool begun = false;
    if (current && !inMetadata && lightingRequested.load() && !lightingAttempted.load())
    {
        FSRD::CyberpunkRayConstants::Invalidate(current->receipt);
        Metadata([&] {
            struct Reader { bool Read(uintptr_t p, void* out, size_t n) noexcept { return ReadExactMemory(p, out, n); } } reader;
            begun = FSRD::CyberpunkRayConstants::Begin(reader, authenticatedImage.load(), caller,
                CurrentRayConstantScope(), bytes, uintptr_t(source), current->receipt);
        });
    }
    originalUploadRayConstants(bytes, source); // Original upload always executes exactly once.
    if (begun)
    {
        Metadata([&] {
            struct Reader { bool Read(uintptr_t p, void* out, size_t n) noexcept { return ReadExactMemory(p, out, n); } } reader;
            if (rayScope != current || !FSRD::CyberpunkRayConstants::Complete(reader, CurrentRayConstantScope(), current->receipt))
                return;
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            if (data.rayConstants.size() == 4) data.rayConstants.erase(data.rayConstants.begin());
            data.rayConstants.push_back(current->receipt); // Source pointer has already been cleared.
        });
        // Metadata exceptions must not preserve a borrowed original stack pointer.
        if (current->receipt.phase == FSRD::CyberpunkRayConstants::Phase::Pending)
            FSRD::CyberpunkRayConstants::Invalidate(current->receipt);
    }
}

FSRD::CyberpunkLightingConstants::Scope CurrentLightingConstantScope()
{
    FSRD::CyberpunkLightingConstants::Scope result;
    uint8_t initialized = 0;
    if (!lightingScope ||
        !ReadEarly(uintptr_t(__readgsqword(0x58)), result.tls) ||
        !ReadEarlyAt(result.tls, 0x14, initialized) || !initialized ||
        !ReadEarlyAt(result.tls, 0x188, result.engine) ||
        !ReadEarlyAt(result.engine, 0x30, result.list))
        throw std::runtime_error("current lighting constant upload TLS/list unavailable");
    result.serial = lightingScope->serial;
    result.graphContext = uintptr_t(lightingScope->context);
    if (!ReadEarlyAt(result.graphContext, 0x18, result.view))
        throw std::runtime_error("current lighting constant view unavailable");
    auto identity = ListIdentity(reinterpret_cast<ID3D12GraphicsCommandList*>(result.list));
    auto& data = Data();
    std::lock_guard lock(data.mutex);
    const auto found = data.lists.find(identity.Get());
    if (found == data.lists.end() || !found->second.known)
        throw std::runtime_error("current lighting constant list generation unavailable");
    result.recordingGeneration = found->second.generation;
    return result;
}

void __fastcall HookUploadLightingConstants(uint32_t bytes, const void* source)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* current = lightingScope;
    FSRD::CyberpunkLightingConstants::Scope observed;
    bool begun = false;
    if (current && !inMetadata && lightingRequested.load() && !lightingAttempted.load())
    {
        FSRD::CyberpunkLightingConstants::Invalidate(current->lightingConstants);
        Metadata([&] {
            if (caller != authenticatedImage.load() + FSRD::CyberpunkLightingConstants::UploadReturnRva) return;
            observed = CurrentLightingConstantScope();
            struct Reader { bool Read(uintptr_t p, void* out, size_t n) { return ReadExactMemory(p, out, n); } } reader;
            begun = FSRD::CyberpunkLightingConstants::Begin(reader, authenticatedImage.load(), caller,
                observed, bytes, uintptr_t(source), current->lightingConstants);
        });
    }
    originalUploadLightingConstants(bytes, source); // Unmodified bytes, exactly one original upload.
    if (begun && current == lightingScope)
        Metadata([&] {
            struct Reader { bool Read(uintptr_t p, void* out, size_t n) { return ReadExactMemory(p, out, n); } } reader;
            FSRD::CyberpunkLightingConstants::Complete(reader, CurrentLightingConstantScope(), current->lightingConstants);
        });
    if (begun && current->lightingConstants.phase == FSRD::CyberpunkLightingConstants::Phase::Pending)
        FSRD::CyberpunkLightingConstants::Invalidate(current->lightingConstants);
}

void __fastcall HookFullscreenHelper(void* renderer, uint32_t shader, uint8_t flag)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* fog = scope;
    const bool previousFog = fog && fog->fogHelper;
    if (fog) fog->fogHelper = !inMetadata && caller == authenticatedImage.load() + FSRD::CyberpunkFogDepth::FullscreenReturnRva;
    struct RestoreFog { Scope* fog; bool previous; ~RestoreFog() { if (fog) fog->fogHelper = previous; } }
        restoreFog { fog, previousFog };
    auto* current = lightingScope;
    const bool previous = current && current->finalHelper;
    const bool previousShaderObserved = current && current->shaderArgumentObserved;
    const uint32_t previousShader = current ? current->shaderArgument : 0;
    if (current)
    {
        current->finalHelper = !inMetadata && caller == authenticatedImage.load() + FinalLightingHelperReturnRva;
        current->shaderArgumentObserved = current->finalHelper;
        current->shaderArgument = current->finalHelper ? shader : 0;
    }
    struct Restore
    {
        LightingScope* current;
        bool previous, previousShaderObserved;
        uint32_t previousShader;
        ~Restore()
        {
            if (!current) return;
            current->finalHelper = previous;
            current->shaderArgumentObserved = previousShaderObserved;
            current->shaderArgument = previousShader;
        }
    } restore { current, previous, previousShaderObserved, previousShader };
    originalFullscreenHelper(renderer, shader, flag); // Scope only; never invoke an extra engine draw.
}

void __fastcall HookBindTextures(uint32_t first, uint32_t count, const uint32_t* handles, uint8_t stage)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* fog = scope;
    uint32_t fogDepth = 0;
    bool fogAdmitted = false;
    if (fog && !inMetadata && stage == 1 && first == 0 && count && fogDepthAuthenticated.load())
    {
        fog->depthBindObserved = false;
        ++fog->depthBindCalls;
        fogAdmitted = fog->depthBindCalls == 1 &&
            FSRD::CyberpunkFogDepth::IsDepthBind(authenticatedImage.load(), caller, first, count, stage) &&
            ReadExactMemory(uintptr_t(handles), &fogDepth, sizeof(fogDepth));
    }
    auto* current = lightingScope;
    uint32_t selected = 0;
    bool admitted = false;
    auto* ray = rayScope;
    RayBindReceipt rayPending;
    bool rayBegun = false;
    if (RayBindingsArmed() && stage == 2)
        Metadata([&] { rayBegun = BeginRayBind(*ray, 0, caller, first, count, handles, rayPending); });
    if (current && !inMetadata && lightingRequested.load() && !lightingAttempted.load() &&
        stage == 1 && first <= 8 && uint64_t(first) + count > 8)
    {
        // ANY later scoped PS t8 write invalidates the one original receipt.
        current->t8BindObserved = false;
        ++current->t8BindCalls;
        admitted = current->t8BindCalls == 1 &&
            FSRD::CyberpunkLightingSource::IsFinalBind(authenticatedImage.load(), caller, first, count, stage) &&
            uintptr_t(handles) <= UINTPTR_MAX - 3 * sizeof(uint32_t) &&
            ReadExactMemory(uintptr_t(handles) + 3 * sizeof(uint32_t), &selected, sizeof(selected));
    }
    originalBindTextures(first, count, handles, stage); // Exactly one untouched native call.
    if (fogAdmitted && fog == scope)
    {
        uint32_t repeated = 0;
        fog->depthBindObserved = ReadExactMemory(uintptr_t(handles), &repeated, sizeof(repeated)) &&
            fogDepth == repeated && fogDepth && fogDepth <= 0x8000;
        fog->depthHandle = fogDepth;
    }
    if (rayBegun)
        Metadata([&] { CompleteRayBind(ray, 0, rayPending, handles); });
    if (admitted && current == lightingScope)
    {
        uint32_t repeated = 0;
        current->t8BindObserved = ReadExactMemory(uintptr_t(handles) + 3 * sizeof(uint32_t), &repeated,
            sizeof(repeated)) && selected == repeated && selected && selected <= 0x8000;
        current->t8Handle = selected; current->t8BindCount = count; current->t8BindCaller = caller;
    }
}

void __fastcall HookBindUavs(uint32_t first, uint32_t count, const uint32_t* handles)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* current = rayScope;
    std::array<RayBindReceipt, 2> pending {};
    std::array<bool, 2> begun {};
    if (RayBindingsArmed())
        for (size_t i = 0; i < begun.size(); ++i)
            Metadata([&] {
                begun[i] = BeginRayBind(*current, i + 1, caller, first, count, handles, pending[i]);
            });
    originalBindUavs(first, count, handles); // Exact three-argument native ABI, exactly once.
    for (size_t i = 0; i < begun.size(); ++i)
        if (begun[i]) Metadata([&] { CompleteRayBind(current, i + 1, pending[i], handles); });
}

bool SameRayCopyScope(const RayCopyBundle& plan) noexcept
{
    try
    {
        const auto& expected = plan.input.dispatch;
        if (!rayScope || rayScope->serial != expected.scope.serial || plan.invalidated || !originalRayCleanup || !active.load() ||
            !captureEnabled.load() || !captureTrackingValid.load() || !rayBindingsAuthenticated.load() ||
            plan.input.image != authenticatedImage.load() || CurrentRayConstantScope() != expected.scope)
            return false;
        uintptr_t object = 0, owner = 0;
        uint32_t hitHandle = 0;
        uint8_t executing = 0;
        UINT width = 0, height = 0;
        if (!ReadEarlyAt(expected.scope.graphContext, 0, object) || object != plan.frameSourceObject ||
            !ReadEarlyAt(expected.scope.graphContext, 0x30, executing) || !(executing & 2) ||
            !ReadEarlyAt(expected.scope.view, 0x34, width) || width != plan.width ||
            !ReadEarlyAt(expected.scope.view, 0x38, height) || height != plan.height ||
            !ReadEarlyAt(expected.scope.view, 0x1d70, owner) || owner != plan.rayOwner ||
            !ReadEarlyAt(owner, 0x274, hitHandle) || hitHandle != expected.textures[2].handle)
            return false;
        for (size_t i = 0; i < rayScope->bindings.size(); ++i)
        {
            // The transparent pass replaces u0 and b6. Our two source handles
            // remain in use until the authenticated common cleanup; u0 is not
            // a source and its old descriptor must not be asserted current.
            if (plan.cleanupEntered && i == 1) continue;
            if (!rayScope->bindings[i].valid || rayScope->bindings[i].scope != expected.scope ||
                rayScope->bindings[i].handle != expected.textures[i].handle)
                return false;
        }
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            const auto found = data.lists.find(plan.listIdentity.Get());
            if (!originalBeginRenderPass || !originalEndRenderPass || found == data.lists.end() ||
                !found->second.known || found->second.generation != expected.scope.recordingGeneration ||
                found->second.predicated || found->second.renderPass || found->second.queryCount)
                return false;
        }
        struct Reader { bool Read(uintptr_t p, void* out, size_t n) noexcept { return ReadExactMemory(p, out, n); } } reader;
        if (plan.cleanupEntered)
        {
            if (rayScope->pendingCopy.get() != &plan || !plan.originalReturned ||
                !plan.completedDispatches || rayScope->nativeDispatchDepth)
                return false;
            // Only current registry source identities are compared here. The
            // selected root layout and descriptors were allowed to change in
            // original code between primary use and this pre-cleanup endpoint.
            uintptr_t registry = 0;
            if (!ReadEarlyAt(plan.input.image, FSRD::CyberpunkEngineAccess::RegistryRva, registry) ||
                registry != expected.registry) return false;
            for (unsigned repeat = 0; repeat < 2; ++repeat)
                for (size_t i : { size_t(0), size_t(2) })
                {
                    const auto& source = expected.textures[i];
                    FSRD::CyberpunkRayBindings::TextureBinding current;
                    if (!FSRD::CyberpunkRayBindings::Detail::ReadTexture(reader, registry, source.handle, i == 2, current) ||
                        current.slot != source.slot || current.native != source.native ||
                        current.descriptor != source.descriptor || current.compact != source.compact ||
                        current.requestedSrvState != source.requestedSrvState ||
                        current.extra != source.extra || current.uavArray != source.uavArray)
                        return false;
                }
            return true;
        }
        const FSRD::CyberpunkRayBindings::TextureHandles handles {
            expected.textures[0].handle, expected.textures[1].handle, expected.textures[2].handle };
        const auto& receipt = rayScope->receipt;
        if (receipt.phase != FSRD::CyberpunkRayConstants::Phase::Uploaded || receipt.source ||
            receipt.scope != expected.scope || receipt.cache != expected.cache || receipt.descriptor != expected.b6.descriptor)
            return false;
        for (unsigned repeat = 0; repeat < 2; ++repeat)
        {
            FSRD::CyberpunkRayBindings::Snapshot current;
            FSRD::CyberpunkRayBindings::Failure reason {};
            if (!FSRD::CyberpunkRayBindings::Detail::ReadCurrent(reader, plan.input.image,
                    expected.list4, expected.scope, receipt, handles, current, reason)) return false;
            current.callerRva = expected.callerRva;
            // ReadCurrent requires positive refs. Ignore ONLY their numeric
            // changes before comparing snapshots: kind3 may legitimately retain.
            // The metadata-only Observe intentionally compares them more strictly.
            for (size_t i = 0; i < current.textures.size(); ++i)
                current.textures[i].refs = expected.textures[i].refs;
            if (current != expected) return false;
        }
        return true;
    }
    catch (...) { return false; }
}

struct RayCopyEngineHost
{
    const RayCopyBundle& plan;
    bool Read(uintptr_t address, void* destination, size_t bytes) noexcept
    { return ReadExactMemory(address, destination, bytes); }
    uint32_t ThreadId() noexcept { return GetCurrentThreadId(); }
    bool ReadTlsSlotZero(uintptr_t& result) noexcept
    { return ReadEarly(uintptr_t(__readgsqword(0x58)), result) && result; }
    bool ExactImageAuthenticated(uintptr_t image, uintptr_t size, uint32_t stamp, std::string_view sha) noexcept
    {
        return active.load() && captureEnabled.load() && image == authenticatedImage.load() &&
            image == uintptr_t(GetModuleHandleW(nullptr)) && size == 0x04efc000 && stamp == 0x68af45ea && sha == ExeSha256;
    }
    bool LiveCodeMatches(uintptr_t image, const FSRD::CyberpunkEngineAccess::CodeRange& code) noexcept
    { return MatchLiveCode(image, code); }
    bool ListIsDirect(uintptr_t list) noexcept
    { return reinterpret_cast<ID3D12GraphicsCommandList*>(list)->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT; }
    bool IsAdmittedPostRayScope(const FSRD::CyberpunkRayAccess::Input& input) noexcept
    {
        // A later lighting request may finish on another recording thread. That
        // must not disable mandatory restoration of this still-valid ray scope.
        return &input == &plan.input && plan.cleanupEntered && plan.originalReturned &&
            plan.work && plan.sources[0] && plan.sources[1] &&
            uintptr_t(plan.sources[0].Get()) == input.dispatch.textures[0].native &&
            uintptr_t(plan.sources[1].Get()) == input.dispatch.textures[2].native && SameRayCopyScope(plan);
    }
    bool IsTextureResidencyAdmitted(uintptr_t registry,
                                    const FSRD::CyberpunkEngineAccess::TextureBorrow& texture) noexcept
    {
        try
        {
            if (registry != plan.input.dispatch.registry) return false;
            for (const auto& code : FSRD::CyberpunkLightingSource::Code)
                if (!MatchLiveCode(plan.input.image, { code.rva, code.bytes, code.sha256 })) return false;
            for (size_t i : { size_t(0), size_t(2) })
            {
                const auto& expected = plan.input.dispatch.textures[i];
                if (texture.handle != expected.handle || texture.native != expected.native) continue;
                FSRD::CyberpunkLightingSource::Snapshot source;
                source.slot = expected.slot; source.native = expected.native;
                if (!ReadEarlyAt(source.slot, 0x68, source.externalSync)) return false;
                FSRD::CyberpunkLightingSource::ResidencySnapshot residency;
                return FSRD::CyberpunkLightingSource::ObserveRegisteredResidency(*this,
                    plan.input.dispatch.scope.engine, source, residency);
            }
        }
        catch (...) {}
        return false;
    }
    void RequestState(uintptr_t address, uintptr_t engine, uint32_t handle, uint32_t state, uint32_t subresource) noexcept
    { reinterpret_cast<void(__fastcall*)(void*, uint32_t, uint32_t, uint32_t)>(address)(reinterpret_cast<void*>(engine), handle, state, subresource); }
    void Flush(uintptr_t address, uintptr_t engine) noexcept
    { reinterpret_cast<void(__fastcall*)(void*)>(address)(reinterpret_cast<void*>(engine)); }
};

std::shared_ptr<RayCopyBundle> PrepareRayCopy(const FSRD::CyberpunkRayBindings::Snapshot& snapshot,
                                           const D3D12_DISPATCH_RAYS_DESC& dispatch, unsigned ordinal)
{
    if (!originalRayCleanup || !rayScope || rayScope->nativeDispatchDepth ||
        privateResetArming.load(std::memory_order_acquire) ||
        !lightingRequested.load() || lightingAttempted.load() ||
        (!TemporalRecordingRequested() && rayCopyAttempted.exchange(true))) return {};
    if (TemporalRecordingRequested() && (ordinal != 1 || rayScope->pendingCopy)) return {};
    auto plan = std::make_shared<RayCopyBundle>();
    plan->input.image = authenticatedImage.load(); plan->input.dispatch = snapshot; plan->dispatchOrdinal = ordinal;
    plan->provenance = { { "status", "preparing" }, { "original_scene_modified", false },
        { "scope", snapshot.scope.serial }, { "view", snapshot.scope.view },
        { "frame_source_cpu", snapshot.scope.frameSource }, { "native_list", snapshot.scope.list },
        { "recording_generation", snapshot.scope.recordingGeneration }, { "dispatch_ordinal", ordinal },
        { "native_dispatch_extent", { dispatch.Width, dispatch.Height, dispatch.Depth } },
        { "stage", "pending_common_ray_node_cleanup_entry_after_all_original_dispatches" },
        { "hit_units", "not_asserted" }, { "final_writer", "not_asserted" },
        { "gpu_completion", "requires_submission_fence" } };
    try
    {
        auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(snapshot.scope.list);
        plan->listIdentity = ListIdentity(list);
        if (!plan->listIdentity || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || dispatch.Depth != 1 ||
            !ReadEarlyAt(snapshot.scope.graphContext, 0, plan->frameSourceObject) || !plan->frameSourceObject ||
            !ReadEarlyAt(snapshot.scope.view, 0x1d70, plan->rayOwner) || !plan->rayOwner ||
            !ReadEarlyAt(snapshot.scope.view, 0x34, plan->width) || !ReadEarlyAt(snapshot.scope.view, 0x38, plan->height) ||
            !plan->width || !plan->height || plan->width > 8192 || plan->height > 8192 ||
            !SameRayCopyScope(*plan))
            throw std::runtime_error("ray-copy current direct-list/view/dispatch extent refused");
        // Adaptive rays can dispatch a flattened1D work domain. The copy extent
        // comes from this exact current view, never DispatchRays Width/Height.
        ComPtr<IUnknown> list4Identity;
        if (FAILED(reinterpret_cast<ID3D12GraphicsCommandList4*>(snapshot.list4)->QueryInterface(IID_PPV_ARGS(&list4Identity))) ||
            list4Identity.Get() != plan->listIdentity.Get() || FAILED(list->GetDevice(IID_PPV_ARGS(&plan->device))))
            throw std::runtime_error("ray-copy native List/List4/device identity differs");
        if (snapshot.textures[0].native == snapshot.textures[1].native ||
            snapshot.textures[2].native == snapshot.textures[1].native)
            throw std::runtime_error("ray-copy source aliases original radiance UAV");
        plan->provenance["source_descriptions"] = Json::array();
        for (size_t i = 0; i < 2; ++i)
        {
            // The exact currently bound original ray t4/u8 are valid engine
            // borrows within this synchronous invocation, before end-use cleanup.
            // This is not a lookup of an old globally sampled native pointer.
            plan->sources[i] = reinterpret_cast<ID3D12Resource*>(snapshot.textures[i ? 2 : 0].native);
            const auto desc = plan->sources[i]->GetDesc();
            plan->provenance["source_descriptions"].push_back({ { "role", i ? "hit" : "motion" },
                { "dimension", unsigned(desc.Dimension) }, { "format", unsigned(desc.Format) },
                { "width", desc.Width }, { "height", desc.Height }, { "depth_or_array_size", desc.DepthOrArraySize },
                { "mip_levels", desc.MipLevels }, { "sample_count", desc.SampleDesc.Count },
                { "sample_quality", desc.SampleDesc.Quality }, { "flags", unsigned(desc.Flags) },
                { "layout", unsigned(desc.Layout) }, { "alignment", desc.Alignment } });
            if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
                throw std::runtime_error("ray-copy simultaneous-access source is unsupported");
        }
        const char* failure = nullptr;
        const auto metadata = Json::parse(FSRDCyberpunkEarlyGuides::Describe(rayScope->context, authenticatedImage.load()));
        auto* window = temporalWindow.load(std::memory_order_acquire);
        auto* packet = window ? SelectTemporalFrame(*window, metadata, plan->device.Get(), WindowPolicy::Role::Ray) :
            privateResetPacket.load(std::memory_order_acquire);
        if (window && !packet) return {}; // Warm-up/stopped/unadmitted frame: no private commands.
        plan->packet = packet;
        if (packet)
        {
            if (rayScope->receipt.words[FSRD::CyberpunkRayConstants::EncodingByteOffset / 4] != 0 ||
                rayScope->receipt.words[FSRD::CyberpunkRayConstants::WriteHitByteOffset / 4] != 1)
                throw std::runtime_error("private RESET requires observed absolute hit encoding and enabled hit write");
            ClaimPrivateReset(*packet, metadata, plan->device.Get(), packet->rayClaimed);
            plan->privateReset = true;
            plan->charge = packet->charge;
            if (plan->charge)
                for (const auto& resource : plan->sources)
                {
                    const auto description = resource->GetDesc();
                    plan->charge->Add(plan->device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes);
                }
            plan->work = FSRD::PrivateRayCopy::PrepareInto(plan->device.Get(), plan->width, plan->height,
                                                          plan->sources, packet->rays, &failure);
            std::lock_guard lock(packet->mutex);
            packet->producerIdentity = plan->listIdentity;
            packet->producer = { uintptr_t(plan->listIdentity.Get()), snapshot.scope.recordingGeneration };
            if (!packet->policy.DeclareProducer(packet->producer))
                throw std::runtime_error("private RESET producer declaration refused");
        }
        else
            plan->work = FSRD::PrivateRayCopy::Prepare(plan->device.Get(), plan->width, plan->height, plan->sources, &failure);
        if (!plan->work || !SameRayCopyScope(*plan))
            throw std::runtime_error(failure && *failure ? failure : "ray-copy source changed during preparation");
        plan->provenance["frame_source_object"] = plan->frameSourceObject;
        plan->provenance["extent"] = { plan->width, plan->height };
        plan->provenance["source_resources"] = { snapshot.textures[0].native, snapshot.textures[2].native };
        plan->provenance["ray_owner"] = plan->rayOwner;
        return plan;
    }
    catch (const std::exception& error) { plan->provenance["reason"] = error.what(); }
    catch (...) { plan->provenance["reason"] = "ray-copy preparation threw"; }
    if (plan->packet) FailPacket(plan->packet);
    else if (auto* window = temporalWindow.load(std::memory_order_acquire))
        StopTemporalWindow(*window, "ray producer preparation refused");
    else FailPrivateReset();
    plan->provenance["status"] = "refused_before_state_requests";
    auto& data = Data();
    std::lock_guard lock(data.mutex);
    data.rayCopyStatus = plan->provenance;
    return {};
}

void FinishRayCopy(const std::shared_ptr<RayCopyBundle>& plan)
{
    if (!plan) return;
    RayCopyEngineHost host { *plan };
    const auto result = FSRD::CyberpunkRayAccess::RecordCopy(host, plan->input, [&] {
        return plan->work->Record(reinterpret_cast<ID3D12GraphicsCommandList*>(plan->input.dispatch.scope.list));
    });
    if (result.outcome == FSRD::CyberpunkRayAccess::Outcome::ScopeLostAfterMutation)
    {
        earlyFatalRecording.store(true); // Must precede any fallible diagnostics.
        try { LOG_ERROR("[FSRRR ray copy] FATAL original scope lost after native state mutation; terminating authenticated Cyberpunk only"); }
        catch (...) {}
        if (active.load() && captureEnabled.load() && plan->input.image == authenticatedImage.load() &&
            plan->input.image == uintptr_t(GetModuleHandleW(nullptr)))
        {
            TerminateProcess(GetCurrentProcess(), 0xf51d0001u);
            RaiseFailFastException(nullptr, nullptr, 0);
        }
        return;
    }
    plan->recorded = result.outcome == FSRD::CyberpunkRayAccess::Outcome::CopyRecordedRestored &&
        result.hitRestored && plan->work->Recorded();
    if (plan->privateReset)
    {
        auto* packet = plan->packet ? plan->packet : privateResetPacket.load(std::memory_order_acquire);
        const auto point = PrivateResetPoint(plan->listIdentity.Get(), plan->input.dispatch.scope.recordingGeneration);
        std::lock_guard lock(packet->mutex);
        if (!plan->recorded) packet->policy.Fail();
        else packet->rayTerminal = point;
    }
    plan->provenance["status"] = plan->recorded ? "private_raw_copy_recorded_hit_restored" : "private_raw_copy_refused_or_failed";
    plan->provenance["outcome"] = unsigned(result.outcome);
    plan->provenance["engine_state_requests"] = result.requestsIssued;
    plan->provenance["hit_uav_restored"] = result.hitRestored;
    plan->provenance["private_output_states"] = { 0xc0, 0xc0 };
    if (!plan->recorded)
        plan->provenance["reason"] = result.outcome == FSRD::CyberpunkRayAccess::Outcome::Refused
            ? "native pre-cleanup scope/source/state admission refused" : plan->work->Error();
    if (plan->packet && plan->packet->temporal)
    {
        std::lock_guard lock(plan->packet->mutex);
        if (plan->recorded) plan->packet->rayCopy = plan; // One immutable bundle per selected frame.
    }
    else
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        data.rayCopyStatus = plan->provenance;
        if (plan->recorded) data.rayCopy = plan;
    }
}

void __fastcall HookRayCleanup(void* context, uint8_t flags)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* current = rayScope;
    const auto plan = current ? current->pendingCopy : std::shared_ptr<RayCopyBundle> {};
    if (plan)
    {
        const bool endpoint = !inMetadata && active.load() && captureEnabled.load() &&
            rayBindingsAuthenticated.load() && plan->input.image == authenticatedImage.load() &&
            caller == plan->input.image + FSRD::CyberpunkRayAccess::CleanupReturnRva &&
            context == current->context && uintptr_t(context) == plan->input.dispatch.scope.graphContext &&
            flags == 0 && !plan->cleanupConsumed && !plan->invalidated && plan->originalReturned &&
            plan->completedDispatches && !current->nativeDispatchDepth;
        plan->cleanupConsumed = true;
        // Any earlier or unexpected cleanup ends this scoped admission, even if
        // a later callback happens to reuse the same handles/native addresses.
        if (!endpoint) plan->invalidated = true;
        plan->cleanupEntered = endpoint;
        Metadata([&] {
            plan->provenance["cleanup_endpoint"] = {
                { "caller_rva", caller >= plan->input.image ? caller - plan->input.image : 0 },
                { "context", uintptr_t(context) }, { "flags", flags },
                { "admitted_entry", endpoint }, { "completed_original_dispatches", plan->completedDispatches },
                { "primary_dispatch_returned", plan->originalReturned },
                { "before_original_unbind_and_graph_end_use", endpoint },
                { "primary_b6_u0_layout_asserted_current", false },
                { "later_node_writers", "not_asserted" } };
            if (endpoint)
            {
                uintptr_t cache = 0, descriptor = 0, layout = 0;
                const auto engine = plan->input.dispatch.scope.engine;
                const bool bindingMetadata = ReadEarlyAt(engine, 0x60, cache) && cache &&
                    ReadEarlyAt(engine, 0x90, descriptor) && descriptor && ReadEarlyAt(cache, 0x68, layout) && layout;
                plan->provenance["cleanup_endpoint"]["current_engine_binding_metadata"] = {
                    { "readable", bindingMetadata }, { "cache", cache }, { "b6_cpu_descriptor_slot", descriptor },
                    { "layout", layout }, { "actual_root_table_correspondence_asserted", false } };
                plan->provenance["stage"] = "common_ray_node_cleanup_entry_after_all_original_dispatches";
                FinishRayCopy(plan); // No original unbind/end-use has run yet.
            }
            else
            {
                plan->provenance["status"] = "refused_before_state_requests";
                plan->provenance["reason"] = "common ray cleanup caller/context/order or scoped source lifetime refused";
                auto& data = Data();
                std::lock_guard lock(data.mutex);
                data.rayCopyStatus = plan->provenance;
            }
        });
        plan->cleanupEntered = false;
    }
    if (earlyFatalRecording.load())
        return; // Fatal post-mutation scope loss must not resume original unbinding.
    originalRayCleanup(context, flags); // Exactly once, only after mandatory hit restoration.
    if (current && current->pendingCopy == plan) current->pendingCopy.reset();
}

Json DescribeRayBinding(const FSRD::CyberpunkRayBindings::Binding& binding)
{
    return { { "register", binding.shaderRegister }, { "cpu_descriptor", binding.descriptor },
        { "descriptor_index", binding.descriptorIndex }, { "map_address", binding.mapAddress },
        { "range_index", binding.rangeIndex }, { "root_parameter", binding.rootParameter },
        { "range_bytes", binding.range } };
}

Json DescribeChangedRaySnapshots(const FSRD::CyberpunkRayBindings::ChangedSnapshots& changed)
{
    if (!changed.available) return nullptr;
    const auto& a = changed.first;
    const auto& b = changed.second;
    Json result { { "scope_equal", a.scope == b.scope },
        { "caller", { a.callerRva, b.callerRva } }, { "list4", { a.list4, b.list4 } },
        { "cache", { a.cache, b.cache } }, { "layout", { a.layout, b.layout } },
        { "descriptor_array", { a.descriptorArray, b.descriptorArray } },
        { "registry", { a.registry, b.registry } },
        { "b6", { DescribeRayBinding(a.b6), DescribeRayBinding(b.b6) } },
        { "textures", Json::array() } };
    for (size_t i = 0; i < a.textures.size(); ++i)
    {
        const auto& x = a.textures[i];
        const auto& y = b.textures[i];
        result["textures"].push_back({ { "register", i == 0 ? 4 : i == 1 ? 0 : 8 },
            { "kind", i == 0 ? "SRV" : "UAV" }, { "equal", x == y },
            { "handle", { x.handle, y.handle } }, { "slot", { x.slot, y.slot } },
            { "refs", { x.refs, y.refs } }, { "native", { x.native, y.native } },
            { "descriptor", { x.descriptor, y.descriptor } },
            { "requested_srv_state", { x.requestedSrvState, y.requestedSrvState } },
            { "extra", { x.extra, y.extra } }, { "uav_array", { x.uavArray, y.uavArray } },
            { "compact", { x.compact, y.compact } },
            { "binding", { DescribeRayBinding(x.binding), DescribeRayBinding(y.binding) } } });
    }
    return result;
}

void WINAPI HookDispatchRays(ID3D12GraphicsCommandList4* list, const D3D12_DISPATCH_RAYS_DESC* description)
{
    const auto caller = uintptr_t(_ReturnAddress());
    auto* current = rayScope;
    std::shared_ptr<RayCopyBundle> rayCopy;
    if (RayBindingsArmed() && current->dispatches < MaxRayDispatches)
    {
        const auto ordinal = ++current->dispatches;
        Metadata([&] {
            Json observation { { "schema", "optiscaler.fsr_rr.ray_dispatch_bindings.v1" },
                { "status", "refused" }, { "scope", current->serial }, { "dispatch_ordinal", ordinal },
                { "native_list4", uintptr_t(list) }, { "native_caller", caller },
                { "gpu_payload_proven", false }, { "resource_readiness_proven", false },
                { "same_frame_pairing", "not_asserted" }, { "resource_ownership", "not_acquired" },
                { "selected_state_object", "not_observed" }, { "gpu_commands_added", false },
                { "observation_scope", "CPU_binding_snapshot_before_original_dispatch_only" },
                { "optional_private_copy", "reported_separately_in_private_ray_copies" } };
            try
            {
                D3D12_DISPATCH_RAYS_DESC desc {};
                static_assert(sizeof(desc) == 104);
                if (!ReadEarly(uintptr_t(description), desc))
                    throw std::runtime_error("native DispatchRays description unavailable");
                // These are GPU virtual addresses only, NEVER CPU-dereferenced.
                observation["dispatch"] = {
                    { "extent", { desc.Width, desc.Height, desc.Depth } },
                    { "ray_generation", { desc.RayGenerationShaderRecord.StartAddress, desc.RayGenerationShaderRecord.SizeInBytes } },
                    { "miss", { desc.MissShaderTable.StartAddress, desc.MissShaderTable.SizeInBytes, desc.MissShaderTable.StrideInBytes } },
                    { "hit_group", { desc.HitGroupTable.StartAddress, desc.HitGroupTable.SizeInBytes, desc.HitGroupTable.StrideInBytes } },
                    { "callable", { desc.CallableShaderTable.StartAddress, desc.CallableShaderTable.SizeInBytes, desc.CallableShaderTable.StrideInBytes } } };
                const auto observed = CurrentRayConstantScope();
                observation["current_scope"] = { { "scope", observed.serial }, { "graph_context", observed.graphContext },
                    { "view", observed.view }, { "tls", observed.tls }, { "engine", observed.engine },
                    { "native_list", observed.list }, { "recording_generation", observed.recordingGeneration },
                    { "frame_source_cpu", observed.frameSource } };
                observation["b6_upload_receipt"] = { { "phase", uint32_t(current->receipt.phase) },
                    { "scope", current->receipt.scope.serial }, { "native_list", current->receipt.scope.list },
                    { "recording_generation", current->receipt.scope.recordingGeneration },
                    { "frame_source_cpu", current->receipt.scope.frameSource },
                    { "return_rva", current->receipt.callerRva }, { "cpu_descriptor", current->receipt.descriptor } };
                observation["original_bind_receipts"] = Json::array();
                bool receiptsMatch = rayScope == current;
                for (size_t i = 0; i < current->bindings.size(); ++i)
                {
                    const auto& binding = current->bindings[i];
                    const bool matches = binding.valid && binding.scope == observed;
                    receiptsMatch &= matches;
                    observation["original_bind_receipts"].push_back({ { "register", RayBindingRegisters[i] },
                        { "kind", i ? "UAV" : "SRV" }, { "handle", binding.handle },
                        { "caller_rva", binding.callerRva }, { "writes_observed", binding.writes },
                        { "expected_call_seen", binding.expectedSeen }, { "matching_scope", matches },
                        { "scope", binding.scope.serial }, { "native_list", binding.scope.list },
                        { "recording_generation", binding.scope.recordingGeneration },
                        { "frame_source_cpu", binding.scope.frameSource } });
                }
                if (!receiptsMatch)
                    throw std::runtime_error("original t4/u0/u8 bind receipts unavailable, overwritten, or scope changed");
                // Check only recording safety, not resource state or GPU completion.
                const auto identity = ListIdentity(reinterpret_cast<ID3D12GraphicsCommandList*>(observed.list));
                {
                    auto& data = Data();
                    std::lock_guard lock(data.mutex);
                    const auto found = data.lists.find(identity.Get());
                    if (!captureTrackingValid.load() || !originalBeginRenderPass || !originalEndRenderPass ||
                        found == data.lists.end() || !found->second.known ||
                        found->second.generation != observed.recordingGeneration || found->second.predicated ||
                        found->second.renderPass || found->second.queryCount)
                        throw std::runtime_error("native ray Reset/predication/query/render-pass metadata unavailable");
                }
                struct Reader { bool Read(uintptr_t p, void* out, size_t n) noexcept { return ReadExactMemory(p, out, n); } } reader;
                FSRD::CyberpunkRayBindings::Snapshot snapshot;
                FSRD::CyberpunkRayBindings::Failure reason {};
                FSRD::CyberpunkRayBindings::ChangedSnapshots changed;
                const FSRD::CyberpunkRayBindings::TextureHandles handles {
                    current->bindings[0].handle, current->bindings[1].handle, current->bindings[2].handle };
                if (!FSRD::CyberpunkRayBindings::Observe(reader, authenticatedImage.load(), caller, uintptr_t(list),
                        observed, current->receipt, handles, snapshot, &reason, &changed))
                {
                    if (changed.available)
                        observation["changed_snapshots"] = DescribeChangedRaySnapshots(changed);
                    throw std::runtime_error(std::string(FSRD::CyberpunkRayBindings::FailureName(reason)));
                }
                if (rayScope != current || CurrentRayConstantScope() != observed)
                    throw std::runtime_error("native ray scope changed after descriptor observation");
                observation["native_caller_rva"] = snapshot.callerRva;
                observation["cache"] = snapshot.cache; observation["layout"] = snapshot.layout;
                observation["descriptor_array"] = snapshot.descriptorArray; observation["registry"] = snapshot.registry;
                observation["b6"] = DescribeRayBinding(snapshot.b6);
                observation["cpu_upload"] = { { "byte_count", FSRD::CyberpunkRayConstants::PayloadBytes },
                    { "return_rva", current->receipt.callerRva }, { "words", current->receipt.words },
                    { "gpu_cbv_bytes_immutable", false } };
                observation["textures"] = Json::array();
                for (size_t i = 0; i < snapshot.textures.size(); ++i)
                {
                    const auto& texture = snapshot.textures[i];
                    observation["textures"].push_back({ { "kind", i ? "UAV" : "SRV" },
                        { "handle", texture.handle }, { "slot", texture.slot }, { "borrowed_native_address", texture.native },
                        { "refs", texture.refs }, { "compact", texture.compact },
                        { "requested_srv_state_metadata", texture.requestedSrvState },
                        { "extra", texture.extra }, { "uav_array", texture.uavArray },
                        { "binding", DescribeRayBinding(texture.binding) } });
                }
                observation["status"] = "original_dispatch_cpu_binding_correspondence_observed";
                rayCopy = PrepareRayCopy(snapshot, desc, ordinal);
            }
            catch (const std::exception& error) { observation["reason"] = error.what(); }
            catch (...) { observation["reason"] = "CPU binding observation threw"; }
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            if (data.rayDispatches.size() == MaxRayDispatches) data.rayDispatches.erase(data.rayDispatches.begin());
            data.rayDispatches.push_back(std::move(observation));
        });
    }
    if (rayCopy && current && rayScope == current) current->pendingCopy = rayCopy;
    const auto pending = current ? current->pendingCopy : std::shared_ptr<RayCopyBundle> {};
    if (pending)
    {
        if (current->nativeDispatchDepth || caller != pending->input.image + FSRD::CyberpunkRayBindings::DispatchReturnRva ||
            uintptr_t(list) != pending->input.dispatch.list4 || pending->completedDispatches >= MaxRayDispatches)
            pending->invalidated = true;
        ++current->nativeDispatchDepth;
    }
    originalDispatchRays(list, description); // Exactly one original call, including all refusals/exceptions.
    if (pending)
    {
        --current->nativeDispatchDepth;
        if (rayScope != current || current->pendingCopy != pending) pending->invalidated = true;
        if (!pending->invalidated)
        {
            ++pending->completedDispatches;
            if (pending == rayCopy) pending->originalReturned = true;
        }
    }
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
        if (count == 1 && rtvs && !dsv && captureEnabled.load() &&
            ((TemporalRecordingRequested() && !lightingAttempted.load()) || FSRDFogLayerCapture::WantsCapture()))
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
    Json depthMetadata;
    FSRD::CyberpunkFogDepth::Snapshot depthSnapshot;
    FSRD::CyberpunkGuidePass::SourceView depthSource;
    ComPtr<ID3D12Resource> depthOutput;
    uintptr_t depthFrameObject = 0;
    uint32_t depthFrame = 0;
    bool depthPrepared = false;
    bool rgbIdentity = false, rgbIdentityPrepared = false;
    bool sceneResetOnce = false, sceneResetWritten = false;
    UINT64 retainedTextureBytes = 0;
    PrivateResetPacket* packet = nullptr;
    bool temporal = false, captureFinal = false;
    std::shared_ptr<TemporalCharge> charge;
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
    FSRD::CyberpunkExposureSource::Snapshot exposureSource {};
    FSRD::CyberpunkGuidePass::SourceView exposureView;
    Json exposureBindings;
    std::shared_ptr<FSRD::CyberpunkExposurePass::Work> exposureWork;
    FSRD::CyberpunkLightingSource::Snapshot lightingT8Source {};
    uintptr_t lightingT8Engine = 0;
    FSRD::CyberpunkGuidePass::SourceView lightingT8View;
    Json lightingT8Bindings;
    bool lightingT8Prepared = false;
    std::shared_ptr<RayCopyBundle> rayCopy; // Private copy Work retained by this capture's keepAlive.
    bool privateReset = false;
    PrivateResetPacket* packet = nullptr;
    std::shared_ptr<TemporalCharge> charge;
};

struct ExposureMemory
{
    bool Read(uintptr_t address, void* destination, size_t bytes) noexcept
    { return ReadExactMemory(address, destination, bytes); }
};

void DescribeLightingConstants(LightingCapturePlan& plan)
{
    auto& evidence = plan.provenance["original_lighting_cb6"];
    evidence = { { "status", "unavailable" }, { "gpu_payload_proven", false },
        { "scope", "original 240-byte CPU upload; descriptor correspondence only" } };
    try
    {
        ExposureMemory memory;
        const auto current = CurrentLightingConstantScope();
        const auto& receipt = lightingScope->lightingConstants;
        FSRD::CyberpunkLightingConstants::Binding binding, repeated;
        if (current.serial != plan.serial || current.list != plan.list || current.recordingGeneration != plan.drawState.generation ||
            receipt.phase != FSRD::CyberpunkLightingConstants::Phase::Uploaded || current != receipt.scope)
            throw std::runtime_error("no matching current original lighting CPU upload receipt");
        evidence["words"] = receipt.words; evidence["byte_count"] = 240;
        evidence["upload_return_rva"] = FSRD::CyberpunkLightingConstants::UploadReturnRva;
        evidence["cpu_descriptor"] = receipt.descriptor;
        evidence["decode_flags_cpu_words"] = { { "optional_decode_byte144", receipt.words[36] },
            { "t8_t10_decode_byte148", receipt.words[37] }, { "t10_enable_byte164", receipt.words[41] },
            { "optional_enable_byte216", receipt.words[54] } };
        const bool corresponds = FSRD::CyberpunkLightingConstants::ObserveBound(memory, current, uintptr_t(plan.pso.Get()), receipt, binding) &&
            FSRD::CyberpunkLightingConstants::ObserveBound(memory, current, uintptr_t(plan.pso.Get()), receipt, repeated) && binding == repeated;
        evidence["descriptor_correspondence"] = corresponds;
        if (corresponds)
            evidence["pixel_cb6_binding"] = { { "layout", binding.layout }, { "cache", binding.cache },
                { "descriptor_array", binding.descriptorArray }, { "descriptor_index", binding.descriptorIndex },
                { "range_index", binding.rangeIndex }, { "native_root_parameter", binding.nativeRootParameter },
                { "range_bytes", binding.range } };
        evidence["status"] = "original_cpu_upload_observed";
        // Reusable descriptor contents/allocation have not been witnessed. These
        // CPU words are evidence, never authority to dispatch or transform t8.
    }
    catch (const std::exception& error) { evidence["reason"] = error.what(); }
}

// Actual original SRV ranges, defaulting to both stages' exposure t37. The
// optional PS-only t8 caller uses the same map, not a cached stale descriptor.
Json ObserveExposureBindings(const LightingCapturePlan& plan, uintptr_t expectedDescriptor,
                             uint32_t reg = 37, uint32_t firstStage = 0)
{
    if ((reg != 37 || firstStage != 0) && (reg != 8 || firstStage != 1))
        throw std::runtime_error("unsupported original lighting SRV observation");
    uintptr_t tls = 0, engine = 0, cache = 0, layout = 0, descriptors = 0, native = 0, pso = 0;
    uint8_t initialized = 0;
    if (!ReadEarly(uintptr_t(__readgsqword(0x58)), tls) || !ReadEarlyAt(tls, 0x14, initialized) || !initialized ||
        !ReadEarlyAt(tls, 0x188, engine) || !ReadEarlyAt(engine, 0x30, native) || native != plan.list ||
        !ReadEarlyAt(engine, 0x3d0, pso) || pso != uintptr_t(plan.pso.Get()) ||
        !ReadEarlyAt(engine, 0x60, cache) || !ReadEarlyAt(cache, 0x68, layout) ||
        !ReadEarlyAt(cache, 0x28, descriptors) || !expectedDescriptor)
        throw std::runtime_error("current exposure binding cache unavailable");
    Json result = { { "tls", tls }, { "engine", engine }, { "cache", cache },
        { "layout", layout }, { "descriptor_array", descriptors }, { "stages", Json::array() } };
    for (uint32_t stage = firstStage; stage < 2; ++stage)
    {
        uint8_t rangeIndex = 0xff;
        std::array<uint8_t, 16> range {};
        uint64_t resources = 0, samplers = 0, dirty70 = 0, dirty78 = 0;
        if (!ReadEarlyAt(layout, 0x4c3 + 2 * (stage * 128 + reg), rangeIndex) || rangeIndex >= 64 ||
            !ReadEarlyAt(layout, 0x38 + uintptr_t(rangeIndex) * 0x10, range) ||
            !ReadEarlyAt(layout, 8, resources) || !ReadEarlyAt(layout, 0, samplers) ||
            !(resources & (uint64_t(1) << rangeIndex)) || (samplers & (uint64_t(1) << rangeIndex)) ||
            !ReadEarlyAt(cache, 0x70, dirty70) || !ReadEarlyAt(cache, 0x78, dirty78) ||
            ((dirty70 | dirty78) & (uint64_t(1) << rangeIndex)))
            throw std::runtime_error("original exposure VS/PS range missing or dirty");
        uint32_t base = 0;
        uint16_t first = 0, count = 0;
        std::memcpy(&base, range.data() + 4, 4);
        std::memcpy(&first, range.data() + 8, 2);
        std::memcpy(&count, range.data() + 10, 2);
        const uint64_t index = uint64_t(base) + reg - first;
        if (reg < first || !count || reg - first >= count || index >= 65536 || range[13] == 2 || range[14] >= 64)
            throw std::runtime_error("original exposure range/descriptor index unsupported");
        uintptr_t descriptor = 0;
        if (!ReadEarlyAt(descriptors, uintptr_t(index) * 8, descriptor) || descriptor != expectedDescriptor)
            throw std::runtime_error("original VS/PS t37 differs from selected exposure SRV");
        result["stages"].push_back({ { "stage", stage == 0 ? "vertex" : "pixel" }, { "register", reg },
            { "range_index", rangeIndex }, { "range_bytes", range }, { "descriptor_index", index },
            { "native_root_parameter", range[14] }, { "cpu_srv_handle", descriptor } });
    }
    return result;
}

uintptr_t CurrentLightingEngine(const LightingCapturePlan& plan)
{
    uintptr_t tls = 0, engine = 0, native = 0, pso = 0;
    uint8_t initialized = 0;
    if (!ReadEarly(uintptr_t(__readgsqword(0x58)), tls) || !ReadEarlyAt(tls, 0x14, initialized) || !initialized ||
        !ReadEarlyAt(tls, 0x188, engine) || !engine ||
        !ReadEarlyAt(engine, 0x30, native) || native != plan.list ||
        !ReadEarlyAt(engine, 0x3d0, pso) || pso != uintptr_t(plan.pso.Get()))
        throw std::runtime_error("current original lighting engine/list/PSO unavailable");
    return engine;
}

bool SameLightingT8Source(const LightingCapturePlan& plan) noexcept
{
    try
    {
        ExposureMemory memory;
        FSRD::CyberpunkLightingSource::Snapshot fresh;
        return lightingScope && lightingScope->serial == plan.serial && lightingScope->t8BindObserved &&
            lightingScope->t8BindCalls == 1 && lightingScope->t8Handle == plan.lightingT8Source.handle &&
            earlyHeapTrackingValid.load() && plan.lightingT8View.resource && plan.lightingT8View.heap &&
            plan.lightingT8Engine && CurrentLightingEngine(plan) == plan.lightingT8Engine &&
            FSRD::CyberpunkLightingSource::Observe(memory, authenticatedImage.load(), plan.context, fresh, plan.lightingT8Engine) &&
            fresh == plan.lightingT8Source && uintptr_t(plan.lightingT8View.resource.Get()) == fresh.native &&
            plan.lightingT8View.descriptor.ptr == fresh.descriptor &&
            ObserveExposureBindings(plan, fresh.descriptor, 8, 1) == plan.lightingT8Bindings;
    }
    catch (...) { return false; }
}

void PrepareLightingT8(LightingCapturePlan& plan)
{
    auto& evidence = plan.provenance["lighting_t8"];
    evidence = { { "status", "refused" }, { "signal_semantics", "encoded original lighting input; raw-ray/undenoised status not proven" },
        { "pixel_transform", "none" }, { "read_state_requirement", 0x8c0 }, { "native_state_exact", false } };
    try
    {
        const auto& shader = plan.provenance.at("lighting_shader");
        if (!shader.at("matched").get<bool>() || shader.at("variant") != "RayTracing_All_NRD" ||
            !lightingScope->t8BindObserved || lightingScope->t8BindCalls != 1)
            throw std::runtime_error("selected All_NRD shader or original PS t8 binder receipt absent");
        ExposureMemory memory;
        for (const auto& code : FSRD::CyberpunkLightingSource::Code)
            if (!MatchLiveCode(authenticatedImage.load(), { code.rva, code.bytes, code.sha256 }))
                throw std::runtime_error("native texture residency live body mismatch");
        plan.lightingT8Engine = CurrentLightingEngine(plan);
        if (!FSRD::CyberpunkLightingSource::Observe(memory, authenticatedImage.load(), plan.context,
                                                   plan.lightingT8Source, plan.lightingT8Engine))
            throw std::runtime_error("current owner t8 resource/ordinary view route refused");
        const auto& source = plan.lightingT8Source;
        if (source.handle != lightingScope->t8Handle)
            throw std::runtime_error("current owner t8 differs from original scoped binder handle");
        plan.lightingT8Bindings = ObserveExposureBindings(plan, source.descriptor, 8, 1);
        for (const auto& target : plan.targets)
            if (source.native == uintptr_t(target.resource.Get())) throw std::runtime_error("t8 writable MRT alias");
        for (const auto& input : plan.textures)
            if (source.native == input.native) throw std::runtime_error("t8 guide/DSV alias");
        // This AddRef is at the authenticated original final draw's CURRENT
        // bound-use boundary, not a registry sample or historical frame pointer.
        plan.lightingT8View.resource = reinterpret_cast<ID3D12Resource*>(source.native);
        plan.lightingT8View.descriptor.ptr = source.descriptor;
        const auto desc = plan.lightingT8View.resource->GetDesc();
        const auto dimensions = plan.metadata.at("view_dimensions").get<std::array<uint32_t, 2>>();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != dimensions[0] ||
            desc.Height != dimensions[1] || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::runtime_error("actual t8 native format/extents unsupported");
        const auto bytes = plan.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (!bytes || bytes > 64ull * 1024 * 1024) throw std::runtime_error("t8 ownership budget exceeded");
        uint64_t generation = 0;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (const auto& heap : data.cpuSrvHeaps)
                if (source.descriptor >= heap.start && (source.descriptor - heap.start) % heap.increment == 0 &&
                    (source.descriptor - heap.start) / heap.increment < heap.count)
                {
                    if (plan.lightingT8View.heap) throw std::runtime_error("t8 CPU descriptor range ambiguous");
                    plan.lightingT8View.heap = heap.heap; generation = heap.generation;
                }
        }
        if (!SameLightingT8Source(plan)) throw std::runtime_error("current t8 changed before capture admission");
        evidence["status"] = "prepared";
        evidence["handle"] = source.handle; evidence["native"] = source.native;
        evidence["view"] = source.view; evidence["owner"] = source.owner; evidence["view_flags"] = source.viewFlags;
        evidence["owner_offset"] = 0x26c; evidence["registry"] = source.registry; evidence["slot"] = source.slot;
        evidence["refs"] = source.refs; evidence["compact_descriptor_bytes"] = source.compact;
        evidence["cpu_srv_handle"] = source.descriptor; evidence["heap_generation"] = generation;
        evidence["binder_return_rva"] = lightingScope->t8BindCaller - authenticatedImage.load();
        evidence["binder_first_register"] = 5; evidence["binder_count"] = lightingScope->t8BindCount;
        evidence["actual_pixel_binding"] = plan.lightingT8Bindings;
        evidence["native_view_format"] = unsigned(desc.Format);
        if (source.externalSync)
        {
            const auto& residency = source.residency;
            evidence["residency"] = { { "status", "already_registered_in_current_original_list" },
                { "engine_context", residency.engineContext }, { "set", residency.set },
                { "object", residency.object }, { "underlying", residency.underlying },
                { "object_bytes", residency.objectBytes }, { "index", residency.index },
                { "count", residency.count }, { "capacity", residency.capacity },
                { "member_address", residency.memberAddress }, { "selected_member_bit", residency.memberBit },
                { "native_insertion_required", false } };
        }
        plan.lightingT8Prepared = true;
    }
    catch (const std::exception& error)
    {
        plan.lightingT8Prepared = false;
        evidence["status"] = "refused"; evidence["reason"] = error.what();
        LOG_WARN("[FSRRR lighting t8] optional capture refused: {}", error.what());
    }
}

bool SameExposureSource(const LightingCapturePlan& plan) noexcept
{
    try
    {
        ExposureMemory memory;
        FSRD::CyberpunkExposureSource::Snapshot fresh;
        return earlyHeapTrackingValid.load() && plan.exposureView.resource && plan.exposureView.heap &&
            FSRD::CyberpunkExposureSource::Observe(memory, authenticatedImage.load(), plan.context, fresh) &&
            fresh == plan.exposureSource && uintptr_t(plan.exposureView.resource.Get()) == fresh.native &&
            plan.exposureView.descriptor.ptr == fresh.descriptor &&
            ObserveExposureBindings(plan, fresh.descriptor) == plan.exposureBindings;
    }
    catch (...) { return false; }
}

void PrepareLightingExposure(LightingCapturePlan& plan)
{
    auto& evidence = plan.provenance["exposure_words"];
    evidence = { { "status", "refused" }, { "normalization", "none" }, { "CPU_exposure_getter_called", false } };
    try
    {
        if (!plan.provenance.at("lighting_shader").at("matched").get<bool>())
            throw std::runtime_error("actual lighting VS/PS/selector identity unmatched");
        for (const auto& code : FSRD::CyberpunkExposureSource::Code)
            if (!MatchLiveCode(authenticatedImage.load(), { code.rva, code.bytes, code.sha256 }))
                throw std::runtime_error("exposure selector/binder/factory live body mismatch");
        ExposureMemory memory;
        if (!FSRD::CyberpunkExposureSource::Observe(memory, authenticatedImage.load(), plan.context, plan.exposureSource))
            throw std::runtime_error("current native exposure selection/structured descriptor refused");
        const auto& source = plan.exposureSource;
        plan.exposureBindings = ObserveExposureBindings(plan, source.descriptor);
        // Original final native draw's actual VS/PS consumption is the ownership
        // boundary for this AddRef, not an arbitrary sampled registry pointer.
        plan.exposureView.resource = reinterpret_cast<ID3D12Resource*>(source.native);
        plan.exposureView.descriptor.ptr = source.descriptor;
        const auto desc = plan.exposureView.resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width < source.byteCount || desc.Width > 1024 * 1024)
            throw std::runtime_error("native exposure buffer extent differs from authored structured view");
        uint64_t generation = 0;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (const auto& heap : data.cpuSrvHeaps)
                if (source.descriptor >= heap.start && (source.descriptor - heap.start) % heap.increment == 0 &&
                    (source.descriptor - heap.start) / heap.increment < heap.count)
                {
                    if (plan.exposureView.heap) throw std::runtime_error("exposure CPU descriptor range ambiguous");
                    plan.exposureView.heap = heap.heap;
                    generation = heap.generation;
                }
        }
        if (!SameExposureSource(plan)) throw std::runtime_error("exposure source changed before descriptor copy");
        auto work = FSRD::CyberpunkExposurePass::Work::Prepare(plan.device.Get(), plan.exposureView);
        if (!work || !SameExposureSource(plan)) throw std::runtime_error("private raw exposure-word pass setup refused");
        evidence["status"] = "prepared";
        evidence["handle"] = source.handle; evidence["native"] = source.native;
        evidence["cpu_srv_handle"] = source.descriptor; evidence["heap_generation"] = generation;
        evidence["registered_bytes"] = source.byteCount; evidence["compact_descriptor_bytes"] = source.compact;
        evidence["selector"] = { { "route", unsigned(source.route) }, { "object", source.object },
            { "vtable", source.vtable }, { "getter_target", source.getterTarget }, { "getter_bytes", source.getterBytes },
            { "mode_owner", source.modeOwner }, { "mode", source.mode }, { "view", source.view },
            { "context_secondary", source.contextSecondary }, { "exposure_owner", source.exposureOwner },
            { "selected_handle_address", source.selectedHandleAddress } };
        evidence["registry"] = { { "address", source.registry }, { "slot", source.slot },
            { "refs", source.refCount }, { "stride", source.stride }, { "first_element", 0 },
            { "memory_kind", source.memoryKind }, { "view_kind", source.viewKind },
            { "external_synchronization", source.externalSynchronization } };
        evidence["actual_bindings"] = plan.exposureBindings;
        evidence["source_byte_offsets"] = { 0, 4, 8, 12, 16, 20, 24 };
        evidence["source_state"] = 0xc0;
        plan.exposureWork = std::move(work);
    }
    catch (const std::exception& error)
    {
        plan.exposureWork.reset();
        evidence["status"] = "refused"; evidence["reason"] = error.what();
        LOG_WARN("[FSRRR lighting exposure] optional capture refused: {}", error.what());
    }
}

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
    bool IsExposureBufferAdmitted(const FSRD::CyberpunkEngineAccess::BufferBorrow& buffer) noexcept
    {
        return plan.exposureWork && buffer.handle == plan.exposureSource.handle &&
            buffer.native == plan.exposureSource.native && SameExposureSource(plan);
    }
    bool IsLightingT8Admitted(const FSRD::CyberpunkEngineAccess::TextureBorrow& texture) noexcept
    {
        return plan.lightingT8Prepared && texture.handle == plan.lightingT8Source.handle &&
            texture.native == plan.lightingT8Source.native && SameLightingT8Source(plan);
    }
    bool IsTextureResidencyAdmitted(uintptr_t registry,
                                    const FSRD::CyberpunkEngineAccess::TextureBorrow& texture) noexcept
    {
        // Only this optional, actually bound original t8 may use the newly
        // authenticated existing-member path. Other guides retain zero-only.
        if (registry != plan.lightingT8Source.registry ||
            !plan.lightingT8Source.residency.memberBit || !IsLightingT8Admitted(texture))
            return false;
        for (const auto& code : FSRD::CyberpunkLightingSource::Code)
            if (!MatchLiveCode(authenticatedImage.load(), { code.rva, code.bytes, code.sha256 })) return false;
        return true;
    }
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
    void RequestBufferState(uintptr_t address, uintptr_t engine, uint32_t handle, uint32_t state) noexcept
    { reinterpret_cast<void(__fastcall*)(void*, uint32_t, uint32_t)>(address)(reinterpret_cast<void*>(engine), handle, state); }
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
    auto* packet = plan.packet ? plan.packet : privateResetPacket.load(std::memory_order_acquire);
    auto work = plan.privateReset
        ? FSRD::CyberpunkGuidePass::PrepareInto(plan.device.Get(), dimensions[0], dimensions[1], sources, pass,
            FSRD::CyberpunkGuideConstants::PackShared(sharedWords), shader, packet->guides)
        : FSRD::CyberpunkGuidePass::Prepare(plan.device.Get(), dimensions[0], dimensions[1], sources, pass,
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
    plan->provenance["lighting_shader"] = current.shaderArgumentObserved
        ? Data().lightingShaders.Describe(current.pso.Get(), current.shaderArgument)
        : Json { { "matched", false }, { "status", "final_helper_argument_not_observed" } };
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
    if (auto* window = temporalWindow.load(std::memory_order_acquire))
    {
        plan->packet = SelectTemporalFrame(*window, plan->metadata, plan->device.Get(), WindowPolicy::Role::Guides);
        if (!plan->packet) return {};
        plan->charge = plan->packet->charge;
    }
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
    if (auto* packet = plan->packet ? plan->packet : privateResetPacket.load(std::memory_order_acquire))
    {
        ClaimPrivateReset(*packet, plan->metadata, plan->device.Get(), packet->guideClaimed);
        plan->privateReset = true;
        plan->packet = packet;
    }
    if (plan->charge)
    {
        plan->charge->Add(ownedBytes);
        for (const auto& target : plan->targets)
        {
            const auto description = target.resource->GetDesc();
            plan->charge->Add(plan->device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes);
        }
    }
    plan->work = PrepareLightingGuideWork(*plan);
    const bool diagnosticFrame = !plan->packet || !plan->packet->temporal || plan->packet->temporalKey.CaptureFinal();
    if (diagnosticFrame)
    {
        PrepareLightingExposure(*plan);
        PrepareLightingT8(*plan);
        DescribeLightingConstants(*plan);
    }
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        auto& candidates = plan->provenance["ray_cpu_upload_candidates"];
        candidates = Json::array();
        for (const auto& receipt : data.rayConstants)
        {
            const auto& observed = receipt.scope;
            candidates.push_back({ { "status", "original_cpu_upload_observed" },
                { "gpu_payload_proven", false }, { "same_frame_pairing", "not_asserted" },
                { "dispatch_binding_proven", false }, { "byte_count", FSRD::CyberpunkRayConstants::PayloadBytes },
                { "upload_return_rva", receipt.callerRva }, { "scope", observed.serial },
                { "graph_context", observed.graphContext }, { "view", observed.view },
                { "tls", observed.tls }, { "engine", observed.engine }, { "native_list", observed.list },
                { "recording_generation", observed.recordingGeneration }, { "frame_source_cpu", observed.frameSource },
                { "same_view_address", observed.view == plan->view }, { "same_list_address", observed.list == plan->list },
                { "cpu_descriptor", receipt.descriptor }, { "words", receipt.words },
                { "hit_encoding_cpu_word", receipt.words[FSRD::CyberpunkRayConstants::EncodingByteOffset / 4] },
                { "write_hit_cpu_word", receipt.words[FSRD::CyberpunkRayConstants::WriteHitByteOffset / 4] } });
        }
        plan->provenance["ray_dispatch_candidates"] = data.rayDispatches;
        plan->provenance["ray_dispatch_observer"] = {
            { "authenticated", rayBindingsAuthenticated.load() },
            { "api_hook_installed", originalDispatchRays != nullptr },
            { "status", "CPU_binding_metadata_only_not_resource_readiness" } };
    }
    plan->provenance["current_inputs"] = plan->metadata;
    plan->provenance["actual_pixel_bindings"] = plan->bindings;
    plan->provenance["native_list"] = plan->list;
    plan->provenance["recording_generation"] = plan->drawState.generation;
    plan->provenance["scope"] = plan->serial;
    plan->provenance["original_mrt_resources"] = { uintptr_t(plan->targets[0].resource.Get()), uintptr_t(plan->targets[1].resource.Get()) };
    plan->provenance["readonly_dsv"] = plan->dsv.ptr;
    return plan;
}

bool RayCopyRecordingMatches(const RayCopyBundle& candidate, const LightingCapturePlan& plan,
                             uintptr_t object, uint32_t frame, const std::array<uint32_t, 2>& dimensions) noexcept
{
    const auto& source = candidate.input.dispatch.scope;
    return candidate.recorded && candidate.work && candidate.work->Recorded() &&
        source.list == plan.list && candidate.listIdentity.Get() == plan.listIdentity.Get() &&
        source.recordingGeneration == plan.drawState.generation && source.view == plan.view &&
        source.serial < plan.serial && candidate.width == dimensions[0] && candidate.height == dimensions[1] &&
        candidate.frameSourceObject == object && source.frameSource == frame;
}

bool AttachRayCopies(LightingCapturePlan& plan, std::array<FSRDFogLayerCapture::Texture, 2>& textures)
{
    auto& evidence = plan.provenance["private_ray_copies"];
    evidence = { { "status", "unavailable" }, { "scene_correction", false },
        { "same_frame", "requires_exact_CPU_source_and_recording_join" } };
    std::shared_ptr<RayCopyBundle> candidate;
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        candidate = data.rayCopy;
        if (!data.rayCopyStatus.is_null()) evidence["copy_attempt"] = data.rayCopyStatus;
    }
    if (plan.packet && plan.packet->temporal)
    {
        std::lock_guard lock(plan.packet->mutex);
        candidate = plan.packet->rayCopy; // Never take a previous frame's global bundle.
        evidence.erase("copy_attempt");
        if (candidate) evidence["copy_attempt"] = candidate->provenance;
    }
    if (!candidate) return false;
    try
    {
        const auto& source = candidate->input.dispatch.scope;
        const auto& frameRoute = plan.metadata.at("camera_provenance").at("frame_id_virtual_route");
        const auto& frame = frameRoute.at("explicit_frame_id_source");
        uintptr_t currentObject = 0;
        uint32_t currentFrame = 0;
        const auto dimensions = plan.metadata.at("view_dimensions").get<std::array<uint32_t, 2>>();
        // The completed bundle must already be published when this exact later
        // lighting callback records on the SAME native list/Reset. This is an
        // ordered private-output dependency, not just a matching resource pointer.
        if (frame.at("status") != "CPU_value_present" || !frame.at("repeated_source_fields_equal").get<bool>() ||
            !RayCopyRecordingMatches(*candidate, plan, frameRoute.at("object_address").get<uintptr_t>(),
                                     frame.at("source_value").get<uint32_t>(), dimensions) ||
            !ReadEarlyAt(plan.context, 0, currentObject) || currentObject != candidate->frameSourceObject ||
            !ReadEarlyAt(currentObject, 0x1b0, currentFrame) || currentFrame != source.frameSource)
            throw std::runtime_error("private ray copy is not an earlier same-list/Reset/view/CPU-frame-source snapshot");
        const auto current = CurrentLightingConstantScope();
        if (current.serial != plan.serial || current.list != plan.list || current.view != plan.view ||
            current.recordingGeneration != source.recordingGeneration)
            throw std::runtime_error("lighting recording changed before private ray readback");
        for (size_t i = 0; i < textures.size(); ++i)
        {
            textures[i].resource = candidate->work->Outputs()[i];
            textures[i].viewFormat = i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            textures[i].state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        evidence["status"] = "earlier_same_recording_CPU_frame_source_join";
        evidence["same_frame"] = "same_exact_current_engine_CPU_source_not_an_observed_SL_token";
        evidence["copy_scope"] = source.serial;
        evidence["lighting_scope"] = plan.serial;
        evidence["original_hit_final_writer"] = "not_asserted";
        evidence["hit_units"] = "not_asserted";
        plan.rayCopy = std::move(candidate);
        return true;
    }
    catch (const std::exception& error) { evidence["reason"] = error.what(); }
    catch (...) { evidence["reason"] = "private ray copy pairing threw"; }
    return false;
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
    if (plan->exposureWork) input.exposure = { plan->exposureSource.handle, plan->exposureSource.native };
    if (plan->lightingT8Prepared) input.lightingT8 = { plan->lightingT8Source.handle, plan->lightingT8Source.native };
    if (!FSRDSubmission::Retain(plan->device.Get(), list, plan))
        throw std::runtime_error("lighting capture lifetime retention unavailable");
    if (plan->privateReset)
    {
        auto* packet = plan->packet ? plan->packet : privateResetPacket.load(std::memory_order_acquire);
        const auto point = PrivateResetPoint(plan->listIdentity.Get(), plan->drawState.generation);
        std::lock_guard lock(packet->mutex);
        packet->guideBegin = point;
    }
    const auto result = FSRD::CyberpunkEngineAccess::RecordPrivateCompute(host, input, [&] {
        return plan->work->Record(list) && (!plan->exposureWork || plan->exposureWork->Record(list));
    });
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
    FSRDFogLayerCapture::Texture exposure;
    if (plan->exposureWork)
    {
        exposure.resource = plan->exposureWork->Output();
        exposure.viewFormat = DXGI_FORMAT_R32G32B32A32_UINT;
        exposure.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        plan->provenance["exposure_words"]["status"] = "private_dispatch_recorded";
    }
    FSRDFogLayerCapture::Texture lightingT8;
    if (plan->lightingT8Prepared)
    {
        lightingT8.resource = plan->lightingT8View.resource;
        lightingT8.viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // Native tracker requested/flushed this read mask; actual can be a
        // compatible superset. Readback records NO input barrier/guessed Before.
        lightingT8.state = D3D12_RESOURCE_STATES(FSRD::CyberpunkEngineAccess::LightingCaptureReadState);
        plan->provenance["lighting_t8"]["status"] = "native_read_mask_requested_and_flushed";
    }
    std::array<FSRDFogLayerCapture::Texture, 2> rayCopies;
    const bool pairedRayCopies = AttachRayCopies(*plan, rayCopies);
    const bool intermediate = plan->packet && plan->packet->temporal && !plan->packet->temporalKey.CaptureFinal();
    if (plan->packet && plan->packet->temporal)
        plan->provenance["temporal_window"] = { { "epoch", plan->packet->temporalKey.epoch },
            { "frame_ordinal", plan->packet->temporalKey.index + 1 }, { "frame_count", 32 },
            { "current_frame", plan->packet->temporalKey.frame }, { "final_capture", !intermediate } };
    const bool recorded = intermediate || FSRDFogLayerCapture::RecordEarlyGuides(plan->device.Get(), list, outputs, plan->provenance.dump(), plan,
        plan->exposureWork ? &exposure : nullptr, plan->lightingT8Prepared ? &lightingT8 : nullptr,
        pairedRayCopies ? &rayCopies : nullptr);
    if (plan->privateReset)
    {
        auto* packet = plan->packet ? plan->packet : privateResetPacket.load(std::memory_order_acquire);
        const auto point = PrivateResetPoint(plan->listIdentity.Get(), plan->drawState.generation);
        std::lock_guard lock(packet->mutex);
        ResetPolicy::ProducerSeal seal;
        seal.ray = packet->producer;
        seal.guides = { uintptr_t(plan->listIdentity.Get()), plan->drawState.generation };
        seal.rayTerminal = packet->rayTerminal; seal.guideBegin = packet->guideBegin;
        seal.guideTerminal = point; seal.sealed = point;
        seal.raySucceeded = pairedRayCopies && plan->rayCopy && plan->rayCopy->privateReset;
        seal.guidesSucceeded = true;
        for (size_t i = 0; i < 3; ++i)
            seal.guidesSucceeded &= plan->work->Outputs()[i].Get() == packet->guides->Outputs()[i].Get();
        seal.rayReadBarriers = seal.raySucceeded;
        seal.guideReadBarriers = seal.guidesSucceeded;
        seal.rayRestored = seal.raySucceeded;
        seal.guidesRestored = result.bindingsRestored;
        seal.rayOwnersRetained = seal.raySucceeded;
        seal.guideOwnersRetained = true;
        if (!recorded || !packet->policy.SealProducer(seal))
            throw std::runtime_error("private RESET producer seal refused");
    }
    if (recorded && pairedRayCopies && !intermediate)
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        data.lightingCaptureSource = { { "scope", plan->serial }, { "metadata", plan->metadata },
            { "native_list", plan->list }, { "recording_generation", plan->drawState.generation },
            { "GPU_completion", "independent lighting capture fence still required" } };
    }
    LOG_INFO("[FSRRR lighting guides] original draw preserved; private guide readback recorded={} scope={}",
             recorded && !intermediate, plan->serial);
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

bool SameFogDepthSource(const CapturePlan& plan) noexcept
{
    try
    {
        const auto& d = plan.depthSnapshot;
        uintptr_t view = 0, object = 0, native = 0, descriptor = 0, bound = 0;
        uintptr_t tls = 0, engine = 0, list = 0, cache = 0, layout = 0, descriptors = 0, pso = 0;
        uint8_t initialized = 0, flags = 0;
        uint32_t frame = 0;
        int32_t refs = 0;
        std::array<uint8_t, 12> compact {};
        if (!scope || !scope->fogHelper || !scope->depthBindObserved || scope->depthBindCalls != 1 ||
            scope->depthHandle != d.handle || scope->serial != d.scope.serial ||
            uintptr_t(scope->context) != d.scope.graphContext || scope->hasDsv || scope->rtvCount != 1 ||
            uintptr_t(scope->psoList) != d.scope.list || uintptr_t(scope->pso) != d.scope.pso ||
            !captureTrackingValid.load() || !earlyHeapTrackingValid.load() ||
            !plan.depthSource.resource || !plan.depthSource.heap ||
            uintptr_t(plan.depthSource.resource.Get()) != d.native || d.native == uintptr_t(plan.main.Get()) ||
            !ReadEarlyAt(d.scope.graphContext, 0x18, view) || view != d.scope.view ||
            !ReadEarlyAt(d.scope.graphContext, 0x30, flags) || !(flags & 2) ||
            !ReadEarly(uintptr_t(__readgsqword(0x58)), tls) || tls != d.scope.tls ||
            !ReadEarlyAt(tls, 0x14, initialized) || !initialized ||
            !ReadEarlyAt(tls, 0x188, engine) || engine != d.scope.engine ||
            !ReadEarlyAt(engine, 0x30, list) || list != d.scope.list ||
            !ReadEarlyAt(engine, 0x3d0, pso) || pso != d.scope.pso ||
            !ReadEarlyAt(engine, 0x60, cache) || cache != d.cache ||
            !ReadEarlyAt(cache, 0x68, layout) || layout != d.layout ||
            !ReadEarlyAt(cache, 0x28, descriptors) || descriptors != d.descriptorArray ||
            !ReadEarlyAt(d.scope.graphContext, 0, object) || object != plan.depthFrameObject ||
            !ReadEarlyAt(object, 0x1b0, frame) || frame != plan.depthFrame ||
            !ReadEarly(d.slot - 8, refs) || refs <= 0 || !ReadEarly(d.slot, native) || native != d.native ||
            !ReadEarlyAt(d.slot, 0x30, descriptor) || descriptor != d.descriptor ||
            !ReadEarlyAt(d.slot, 0x4e, compact) || compact != d.compact ||
            !ReadEarlyAt(d.descriptorArray, uintptr_t(d.descriptorIndex) * 8, bound) || bound != d.descriptor)
            return false;
        // Private native copies do not replace descriptors. Reentry may dirty
        // ranges, so clean-cache bits are only checked by initial Observe().
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        const auto found = data.lists.find(plan.endpoint->list.Get());
        return found != data.lists.end() && found->second.known &&
            found->second.generation == d.scope.recordingGeneration && !found->second.predicated &&
            !found->second.renderPass && !found->second.queryCount;
    }
    catch (...) { return false; }
}

struct FogDepthEngineHost
{
    const CapturePlan& plan;
    bool Read(uintptr_t p, void* out, size_t n) noexcept { return ReadExactMemory(p, out, n); }
    uint32_t ThreadId() noexcept { return GetCurrentThreadId(); }
    bool ReadTlsSlotZero(uintptr_t& result) noexcept
    { return ReadEarly(uintptr_t(__readgsqword(0x58)), result) && result; }
    bool ExactImageAuthenticated(uintptr_t image, uintptr_t size, uint32_t stamp, std::string_view sha) noexcept
    {
        return active.load() && captureEnabled.load() && fogDepthAuthenticated.load() &&
            image == authenticatedImage.load() && image == uintptr_t(GetModuleHandleW(nullptr)) &&
            size == 0x04efc000 && stamp == 0x68af45ea && sha == ExeSha256;
    }
    bool LiveCodeMatches(uintptr_t image, const FSRD::CyberpunkEngineAccess::CodeRange& code) noexcept
    { return MatchLiveCode(image, code); }
    bool IsAdmittedFogDenoiseScope(const FSRD::CyberpunkFogDenoiseAccess::Input& input) noexcept
    {
        return input.list == plan.depthSnapshot.scope.list && input.originalPso == plan.depthSnapshot.scope.pso &&
            input.originalFogScope == plan.depthSnapshot.scope.serial && input.depth.handle == plan.depthSnapshot.handle &&
            input.depth.native == plan.depthSnapshot.native && SameFogDepthSource(plan);
    }
    bool IsTextureResidencyAdmitted(uintptr_t registry, const FSRD::CyberpunkEngineAccess::TextureBorrow& input) noexcept
    {
        const auto& d = plan.depthSnapshot;
        if (registry != d.registry || input.handle != d.handle || input.native != d.native || !SameFogDepthSource(plan)) return false;
        for (const auto& code : FSRD::CyberpunkLightingSource::Code)
            if (!MatchLiveCode(authenticatedImage.load(), { code.rva, code.bytes, code.sha256 })) return false;
        FSRD::CyberpunkLightingSource::Snapshot source;
        source.slot = d.slot; source.native = d.native; source.externalSync = d.residencyUnderlying;
        FSRD::CyberpunkLightingSource::ResidencySnapshot observed;
        return FSRD::CyberpunkLightingSource::ObserveRegisteredResidency(*this, d.scope.engine, source, observed);
    }
    bool ListIsDirect(uintptr_t list) noexcept
    { return reinterpret_cast<ID3D12GraphicsCommandList*>(list)->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT; }
    uintptr_t CurrentNativeList(uintptr_t p) noexcept { return uintptr_t(reinterpret_cast<void*(__fastcall*)()>(p)()); }
    void RequestState(uintptr_t p, uintptr_t engine, uint32_t handle, uint32_t state, uint32_t sub) noexcept
    { reinterpret_cast<void(__fastcall*)(void*, uint32_t, uint32_t, uint32_t)>(p)(reinterpret_cast<void*>(engine), handle, state, sub); }
    void Flush(uintptr_t p, uintptr_t engine) noexcept
    { reinterpret_cast<void(__fastcall*)(void*)>(p)(reinterpret_cast<void*>(engine)); }
    void Reenter(uintptr_t p, uintptr_t list) noexcept
    { reinterpret_cast<void(__fastcall*)(ID3D12GraphicsCommandList*)>(p)(reinterpret_cast<ID3D12GraphicsCommandList*>(list)); }
    void RestorePso(uintptr_t list, uintptr_t pso) noexcept
    { originalSetPso(reinterpret_cast<ID3D12GraphicsCommandList*>(list), reinterpret_cast<ID3D12PipelineState*>(pso)); }
    void FlushGraphicsTables(uintptr_t p, uintptr_t cache, uintptr_t engine) noexcept
    {
        reinterpret_cast<void(__fastcall*)(void*, void*, bool)>(p)(
            reinterpret_cast<void*>(cache), reinterpret_cast<void*>(engine), false);
    }
    bool OriginalFogDepthTableRestored(uintptr_t cache, uintptr_t engine) noexcept
    {
        const auto& d = plan.depthSnapshot;
        uint8_t rangeIndex = 0;
        std::array<uint8_t, 16> range {};
        uint64_t dirty70 = 0, dirty78 = 0;
        return cache == d.cache && engine == d.scope.engine && d.rangeIndex < 64 &&
            SameFogDepthSource(plan) && ReadEarlyAt(d.layout, 0x5c3, rangeIndex) && rangeIndex == d.rangeIndex &&
            ReadEarlyAt(d.layout, 0x38 + uintptr_t(d.rangeIndex) * 16, range) && range == d.range &&
            ReadEarlyAt(cache, 0x70, dirty70) && ReadEarlyAt(cache, 0x78, dirty78) &&
            !((dirty70 | dirty78) & (uint64_t(1) << d.rangeIndex));
    }
};

bool IsFullRgbViewport(const ListState& state, UINT width, UINT height) noexcept
{
    if (state.viewportCount != 1 || state.scissorCount != 1 || !width || !height || width > 8192 || height > 8192)
        return false;
    const auto exact = [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); };
    const auto& v = state.viewports[0];
    const auto& r = state.scissors[0];
    return exact(v.TopLeftX, 0) && exact(v.TopLeftY, 0) && exact(v.Width, float(width)) &&
        exact(v.Height, float(height)) && exact(v.MinDepth, 0) && exact(v.MaxDepth, 1) &&
        r.left == 0 && r.top == 0 && r.right == LONG(width) && r.bottom == LONG(height);
}

bool SameFogRgbTarget(const CapturePlan& plan, UINT width, UINT height, IUnknown* deviceIdentity) noexcept
{
    try
    {
        if (!deviceIdentity || privateResetArming.load(std::memory_order_acquire) ||
            !plan.privateHeap || !plan.frozenOriginalRtv.ptr || !originalSetRtv || !originalSetPso ||
            !SameFogDepthSource(plan)) return false;
        const auto& d = plan.depthSnapshot;
        const auto& current = *scope;
        const auto& bound = current.boundRtv;
        const auto& view = plan.originalView;
        uint32_t topology = 0;
        ComPtr<IUnknown> identity;
        if (!MatchLiveCode(authenticatedImage.load(), FogTopologyCode) ||
            !ReadEarlyAt(d.scope.engine, 0x628, topology) || topology != 4 ||
            current.rtvList != reinterpret_cast<ID3D12GraphicsCommandList*>(d.scope.list) ||
            current.rtvs[0].ptr != plan.originalRtv.ptr || !bound.known || bound.resource.Get() != plan.main.Get() ||
            bound.heap.Get() != plan.sourceHeap.Get() || bound.view.Format != view.Format ||
            bound.view.ViewDimension != view.ViewDimension || bound.view.Texture2D.MipSlice != view.Texture2D.MipSlice ||
            bound.view.Texture2D.PlaneSlice != view.Texture2D.PlaneSlice ||
            FAILED(plan.device->QueryInterface(IID_PPV_ARGS(&identity))) || !identity || identity.Get() != deviceIdentity)
            return false;
        // Scope holds the actual original-use RTV resource/view, not a reread of
        // its reusable CPU slot. The approved helper changes no IA/RS/VRS; the
        // exact original20cc64→1f6fbc path and repeated engine cache4 witness
        // TRIANGLELIST. Unsupported external untracked IA mutation is not admitted.
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        const auto found = data.lists.find(plan.endpoint->list.Get());
        return found != data.lists.end() && found->second.known && !found->second.predicated &&
            !found->second.renderPass && !found->second.queryCount && found->second.generation == plan.drawState.generation &&
            IsFullRgbViewport(found->second, width, height);
    }
    catch (...) { return false; }
}

bool SameFogRgbSource(const CapturePlan& plan) noexcept
{
    auto* packet = rgbIdentityPacket.load(std::memory_order_acquire);
    return packet && !privateResetPacket.load(std::memory_order_acquire) &&
        plan.rgbIdentity && plan.rgbIdentityPrepared && plan.layers.before.resource &&
        plan.layers.before.state == D3D12_RESOURCE_STATES(0xc0) && plan.layers.rgbIdentity.resource &&
        SameFogRgbTarget(plan, packet->width, packet->height, packet->deviceIdentity.Get());
}

struct FogRgbEngineHost : FogDepthEngineHost
{
    explicit FogRgbEngineHost(const CapturePlan& value) : FogDepthEngineHost { value } {}
    bool targetRestored = false;
    bool IsAdmittedFogRgbScope(const FSRD::CyberpunkFogDenoiseAccess::Input& input) noexcept
    { return !input.copySource && SameFogRgbSource(plan); }
    void RestoreOriginalFogTarget(uintptr_t list) noexcept
    {
        // Real OM setter, not an engine-cache assignment. One frozen equivalent
        // original RTV, no DSV, no color write. Bypasses diagnostic hook tracking.
        originalSetRtv(reinterpret_cast<ID3D12GraphicsCommandList*>(list), 1, &plan.frozenOriginalRtv, FALSE, nullptr);
        targetRestored = true;
    }
};

void RecordRgbIdentity(ID3D12GraphicsCommandList* list, CapturePlan& plan)
{
    if (!plan.rgbIdentity) return;
    auto& evidence = plan.provenance["rgb_identity_control"];
    bool snapshotStarted = false;
    bool privateStatesKnown = true;
    try
    {
        if (!plan.rgbIdentityPrepared || !plan.depthPrepared || !SameFogDepthSource(plan) ||
            plan.layers.before.state != D3D12_RESOURCE_STATE_COPY_DEST ||
            plan.layers.rgbIdentity.state != D3D12_RESOURCE_STATE_COPY_DEST)
            throw std::runtime_error("RGB identity original Fog depth/scope unavailable");
        auto* packet = rgbIdentityPacket.load(std::memory_order_acquire);
        if (!packet) throw std::runtime_error("RGB identity explicit packet unavailable");
        const auto& original = plan.depthSnapshot.scope;
        evidence["scope"] = original.serial;
        evidence["view"] = original.view;
        evidence["frame_source_object"] = plan.depthFrameObject;
        evidence["frame_source_value"] = plan.depthFrame;
        evidence["list"] = original.list;
        evidence["recording_generation"] = original.recordingGeneration;
        evidence["render_extent"] = { packet->width, packet->height };
        evidence["source_snapshot_address"] = uintptr_t(plan.layers.before.resource.Get());
        evidence["target_address"] = uintptr_t(plan.main.Get());
        evidence["identity_snapshot_address"] = uintptr_t(plan.layers.rgbIdentity.resource.Get());
        const char* error = nullptr;
        // Work is local, not owned by the CapturePlan retained through readback.
        // Its separate leaf lease retains resources/root/PSO before any RGB command.
        auto work = FSRD::CyberpunkFogRgbWrite::Prepare(plan.device.Get(), packet->width, packet->height,
            plan.layers.before.resource, plan.main, plan.originalView, plan.drawState.viewports[0],
            plan.drawState.scissors[0], &error);
        if (!work) throw std::runtime_error(error && *error ? error : "RGB identity preparation refused");
        // CopyMain already captured this exact same-list pre-Fog image and
        // restored main RT. Only this OWNED source receives a private barrier.
        privateStatesKnown = false;
        Transition(list, plan.layers.before.resource.Get(), plan.layers.before.state, D3D12_RESOURCE_STATES(0xc0));
        plan.layers.before.state = D3D12_RESOURCE_STATES(0xc0);
        privateStatesKnown = true;
        FogRgbEngineHost host { plan };
        FSRD::CyberpunkFogDenoiseAccess::Input input;
        input.image = authenticatedImage.load(); input.list = uintptr_t(list);
        input.originalPso = uintptr_t(plan.originalPso.Get()); input.originalFogScope = plan.depthSnapshot.scope.serial;
        input.depth = { plan.depthSnapshot.handle, plan.depthSnapshot.native };
        const auto result = FSRD::CyberpunkFogDenoiseAccess::RecordSceneRgb(host, input, [&] { return work->Record(list); });
        if (result.outcome == FSRD::CyberpunkFogDenoiseAccess::SceneOutcome::ScopeLostAfterMutation ||
            (result.bindingsRestored && !host.targetRestored))
        {
            plan.fatalEarlyRecording = true;
            PrivateResetFatal(); // Terminal latch precedes logging/JSON; never resume a corrupt Fog recording.
        }
        evidence["outcome"] = unsigned(result.outcome);
        evidence["callback_entered"] = result.callbackEntered;
        evidence["bindings_restored"] = result.bindingsRestored;
        evidence["original_target_restored"] = host.targetRestored;
        evidence["draw_recorded"] = work->Recorded();
        evidence["identity_passed"] = nullptr; // Only actual raw before/snapshot comparison can establish this.
        if (!result.bindingsRestored || !result.callbackEntered)
            throw std::runtime_error("RGB identity scene admission refused before draw");
        // Even a callback failure can have partial RGB writes. Capture them for
        // the control, never report rollback/success/retry or run a second denoiser.
        snapshotStarted = true;
        privateStatesKnown = false;
        CopyMain(list, plan, plan.layers.rgbIdentity.resource.Get());
        Transition(list, plan.layers.rgbIdentity.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATES(0xc0));
        plan.layers.rgbIdentity.state = D3D12_RESOURCE_STATES(0xc0);
        privateStatesKnown = true;
        evidence["status"] = result.outcome == FSRD::CyberpunkFogDenoiseAccess::SceneOutcome::SceneRecordedRestored
            ? "rgb_draw_and_pre_fog_snapshot_recorded" : "rgb_draw_failed_restored_snapshot_recorded";
        evidence["snapshot_order"] = "original pre-Fog copy -> RGB-only identity -> snapshot -> original Fog once";
    }
    catch (const std::exception& error)
    {
        // Do not pass a potentially half-transitioned private resource into
        // later readback. The outer caller abandons capture; its prior retention
        // still owns every recorded resource, and CopyMain restores native RT.
        if (!privateStatesKnown) throw;
        // Before snapshot recording this optional owner is unused and can be
        // dropped. After any copy attempt keep it in the already-retained plan,
        // even if later readback refuses its incomplete state.
        if (!snapshotStarted) plan.layers.rgbIdentity = {};
        evidence["status"] = "identity_control_refused_or_incomplete";
        evidence["reason"] = error.what();
    }
}

void PrepareFogDepth(CapturePlan& plan, uintptr_t caller)
{
    auto& evidence = plan.provenance["hardware_depth"];
    evidence = { { "schema", "optiscaler.fsr_rr.fog_hardware_depth.v1" }, { "status", "refused" },
        { "pixel_transform", "none; native R32_FLOAT copy" }, { "original_scene_modified", false } };
    try
    {
        if (!fogDepthAuthenticated.load() || !scope || !scope->fogHelper || !scope->depthBindObserved ||
            scope->depthBindCalls != 1) throw std::runtime_error("exact Fog helper/t0 binder unavailable");
        if (earlyRequested.load()) throw std::runtime_error("legacy initializer capture and independent Fog depth capture are mutually exclusive");
        plan.depthMetadata = Json::parse(FSRDCyberpunkEarlyGuides::Describe(scope->context, authenticatedImage.load()));
        plan.provenance["current_inputs"] = plan.depthMetadata;
        plan.depthFrame = EarlyFrameSource(plan.depthMetadata);
        if (!ReadEarlyAt(uintptr_t(scope->context), 0, plan.depthFrameObject) || !plan.depthFrameObject)
            throw std::runtime_error("current Fog frame source object unavailable");
        if (plan.depthFrameObject != plan.depthMetadata.at("camera_provenance").at("frame_id_virtual_route").at("object_address").get<uintptr_t>())
            throw std::runtime_error("Fog frame object changed since metadata observation");
        const auto& selected = plan.depthMetadata.at("inputs").at(5);
        const auto& interval = selected.at("logical_interval");
        if (selected.at("base_key").get<uint32_t>() != FSRD::CyberpunkFogDepth::GraphKey ||
            selected.at("status") != "handle_present" || selected.at("handle").get<uint32_t>() != scope->depthHandle ||
            interval.at("status") != "compiler_interval_observed" || !interval.at("repeated_metadata_equal").get<bool>() ||
            !interval.at("inclusive_contains_position").get<bool>() || interval.at("end_event_relation") != "before" ||
            interval.at("graph_phase").get<unsigned>() != 2 || interval.at("record_used_flag").get<unsigned>() != 1 ||
            interval.at("record_handle") != selected.at("handle") ||
            interval.at("holder_first_use").get<uint64_t>() > interval.at("current_position").get<uint64_t>() ||
            interval.at("current_position").get<uint64_t>() >= interval.at("holder_end_event_position").get<uint64_t>() ||
            interval.at("holder_end_event_position").get<uint64_t>() > interval.at("holder_reservation_end").get<uint64_t>())
            throw std::runtime_error("current original Fog depth graph reservation refused");
        FSRD::CyberpunkFogDepth::Scope current;
        current.serial = scope->serial; current.recordingGeneration = plan.drawState.generation;
        current.graphContext = uintptr_t(scope->context); current.view = plan.depthMetadata.at("view").get<uintptr_t>();
        current.list = uintptr_t(scope->psoList); current.pso = uintptr_t(scope->pso);
        if (!ReadEarly(uintptr_t(__readgsqword(0x58)), current.tls) || !ReadEarlyAt(current.tls, 0x188, current.engine))
            throw std::runtime_error("current Fog TLS unavailable");
        ExposureMemory memory;
        FSRD::CyberpunkFogDepth::Failure failure;
        if (!FSRD::CyberpunkFogDepth::Observe(memory, authenticatedImage.load(), caller, current, scope->depthHandle,
                                             plan.depthSnapshot, &failure))
            throw std::runtime_error(std::string(FSRD::CyberpunkFogDepth::FailureName(failure)));
        const auto& d = plan.depthSnapshot;
        if (d.native != selected.at("texture_registry").at("borrowed_native_address").get<uintptr_t>() ||
            d.native == uintptr_t(plan.main.Get())) throw std::runtime_error("Fog depth resource/scene alias refused");
        // The original Fog draw currently consumes this exact ordinary t0 view.
        // Acquire ownership here, not from a prior frame's surviving descriptor.
        plan.depthSource.resource = reinterpret_cast<ID3D12Resource*>(d.native);
        plan.depthSource.descriptor.ptr = d.descriptor;
        ComPtr<IUnknown> sourceId, mainId;
        if (FAILED(plan.depthSource.resource.As(&sourceId)) || FAILED(plan.main.As(&mainId)) || sourceId.Get() == mainId.Get())
            throw std::runtime_error("canonical Fog depth/scene identity refused");
        const auto desc = plan.depthSource.resource->GetDesc();
        evidence["native_description"] = { { "format", UINT(desc.Format) }, { "flags", UINT(desc.Flags) },
            { "width", desc.Width }, { "height", desc.Height }, { "mips", desc.MipLevels },
            { "array", desc.DepthOrArraySize }, { "samples", desc.SampleDesc.Count } };
        const auto color = plan.main->GetDesc();
        const auto dimensions = plan.depthMetadata.at("view_dimensions").get<std::array<uint32_t, 2>>();
        const auto copyKind = FSRD::CyberpunkFogDepthCopy::ClassifySource(desc, d.srvFormat,
                                                                        dimensions[0], dimensions[1]);
        if (copyKind == FSRD::CyberpunkFogDepthCopy::SourceKind::Refused ||
            desc.Width != color.Width || desc.Height != color.Height)
            throw std::runtime_error("native Fog depth is not the admitted scalar single-plane copy format");
        evidence["copy_source_kind"] = copyKind == FSRD::CyberpunkFogDepthCopy::SourceKind::TypelessR32Depth
            ? "R32_TYPELESS_depth_with_original_R32_FLOAT_SRV" : "typed_R32_FLOAT";
        evidence["source_view_format"] = d.srvFormat;
        evidence["copy_region"] = "whole_subresource0; destination_offsets_0; null_source_box; no_conversion";
        ComPtr<ID3D12Device> device;
        ComPtr<IUnknown> sourceDeviceId, targetDeviceId;
        if (FAILED(plan.depthSource.resource->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device.As(&sourceDeviceId)) || FAILED(plan.device.As(&targetDeviceId)) ||
            sourceDeviceId.Get() != targetDeviceId.Get())
            throw std::runtime_error("Fog depth device mismatch");
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (const auto& heap : data.cpuSrvHeaps)
                if (d.descriptor >= heap.start && (d.descriptor - heap.start) % heap.increment == 0 &&
                    (d.descriptor - heap.start) / heap.increment < heap.count)
                {
                    if (plan.depthSource.heap) throw std::runtime_error("ambiguous current depth CPU heap");
                    plan.depthSource.heap = heap.heap;
                }
        }
        if (!plan.depthSource.heap) throw std::runtime_error("current depth CPU heap not retained");
        const auto repeated = Json::parse(FSRDCyberpunkEarlyGuides::Describe(scope->context, authenticatedImage.load()));
        if (repeated.at("inputs").at(5) != selected || repeated.at("camera_provenance") != plan.depthMetadata.at("camera_provenance") ||
            repeated.at("view") != plan.depthMetadata.at("view") || !SameFogDepthSource(plan))
            throw std::runtime_error("current Fog graph/camera/depth changed during preparation");
        auto output = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, desc.Width, desc.Height, 1, 1);
        if (!FSRD::CyberpunkFogDepthCopy::AdmitDestination(output, dimensions[0], dimensions[1]))
            throw std::runtime_error("private native Fog depth destination descriptor refused");
        const auto sourceBytes = plan.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        const auto outputBytes = plan.device->GetResourceAllocationInfo(0, 1, &output).SizeInBytes;
        if (!sourceBytes || !outputBytes || sourceBytes > MaxCaptureTextureBytes || outputBytes > MaxCaptureTextureBytes ||
            plan.retainedTextureBytes > MaxCaptureTextureBytes - sourceBytes ||
            outputBytes > MaxCaptureTextureBytes - plan.retainedTextureBytes - sourceBytes)
            throw std::runtime_error("Fog depth shared retained-allocation budget exceeded");
        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(plan.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&plan.depthOutput))))
            throw std::runtime_error("private Fog depth allocation failed");
        plan.depthPrepared = true;
        plan.retainedTextureBytes += sourceBytes + outputBytes;
        evidence["status"] = "prepared_at_original_use";
        evidence["source"] = { { "handle", d.handle }, { "native", d.native }, { "descriptor", d.descriptor },
            { "pixel_register", 0 }, { "scope", current.serial }, { "list", current.list },
            { "recording_generation", current.recordingGeneration }, { "view", current.view },
            { "frame_source_object", plan.depthFrameObject }, { "frame_source_cpu", plan.depthFrame } };
    }
    catch (const std::exception& error)
    {
        // Preparation has issued no depth commands. Do not retain rejected
        // sources (especially budget refusals) in the later Fog submission.
        plan.depthPrepared = false;
        plan.depthSource = {};
        plan.depthOutput.Reset();
        evidence["reason"] = error.what();
    }
}

void RecordFogDepth(ID3D12GraphicsCommandList* list, CapturePlan& plan)
{
    if (!plan.depthPrepared) return;
    FogDepthEngineHost host { plan };
    FSRD::CyberpunkFogDenoiseAccess::Input input;
    input.image = authenticatedImage.load(); input.list = uintptr_t(list);
    input.originalPso = uintptr_t(plan.originalPso.Get()); input.originalFogScope = plan.depthSnapshot.scope.serial;
    input.depth = { plan.depthSnapshot.handle, plan.depthSnapshot.native }; input.copySource = true;
    const auto result = FSRD::CyberpunkFogDenoiseAccess::RecordPrivateCompute(host, input, [&] {
        const CD3DX12_TEXTURE_COPY_LOCATION source(plan.depthSource.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION target(plan.depthOutput.Get(), 0);
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        Transition(list, plan.depthOutput.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return true;
    });
    if (result.outcome == FSRD::CyberpunkFogDenoiseAccess::Outcome::ScopeLostAfterMutation)
    {
        earlyFatalRecording.store(true);
        plan.fatalEarlyRecording = true;
        if (active.load() && captureEnabled.load() && input.image == authenticatedImage.load() &&
            input.image == uintptr_t(GetModuleHandleW(nullptr)))
        {
            TerminateProcess(GetCurrentProcess(), 0xf51d0001u);
            RaiseFailFastException(nullptr, nullptr, 0);
        }
        return;
    }
    auto& evidence = plan.provenance["hardware_depth"];
    evidence["state_requests"] = result.requestsIssued;
    evidence["bindings_restored"] = result.bindingsRestored;
    if (result.outcome == FSRD::CyberpunkFogDenoiseAccess::Outcome::PrivateRecordedRestored)
    {
        plan.layers.hardwareDepth.resource = plan.depthOutput;
        plan.layers.hardwareDepth.viewFormat = DXGI_FORMAT_R32_FLOAT;
        plan.layers.hardwareDepth.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        evidence["status"] = "private_copy_recorded";
        evidence["recording_position"] = "immediately_before_original_Fog_draw_on_its_own_list";
    }
    else
    {
        evidence["status"] = "native_access_refused";
        // No successful callback means no copied depth pixels. Keep any recorded
        // resource references retained by the caller; do not publish a companion.
        plan.depthPrepared = false;
    }
}

struct FogSceneResetEngineHost : FogRgbEngineHost
{
    explicit FogSceneResetEngineHost(const CapturePlan& value) : FogRgbEngineHost(value) {}
    bool IsAdmittedFogRgbScope(const FSRD::CyberpunkFogDenoiseAccess::Input& input) noexcept
    {
        auto* packet = plan.packet ? plan.packet : privateResetPacket.load(std::memory_order_acquire);
        if (input.copySource || !FSRD::PreFogSession::LateSrOnly() || !packet ||
            (!packet->sceneResetOnce && !packet->temporal) ||
            rgbIdentityPacket.load(std::memory_order_acquire) || plan.rgbIdentity || (!plan.sceneResetOnce && !plan.temporal) ||
            plan.sceneResetWritten || !packet->denoise || !packet->denoise->Recorded() ||
            !plan.layers.privateReset[2].resource || plan.layers.privateReset[2].state != D3D12_RESOURCE_STATES(0xc0) ||
            plan.layers.privateReset[2].resource.Get() != packet->denoise->Outputs().composed.Get() ||
            packet->consumerIdentity.Get() != plan.endpoint->list.Get()) return false;
        {
            std::lock_guard lock(packet->mutex);
            if (!packet->fogClaimed || packet->policy.Failed()) return false;
        }
        return SameFogRgbTarget(plan, packet->width, packet->height, packet->deviceIdentity.Get());
    }
};

void RecordSceneReset(ID3D12GraphicsCommandList* list, CapturePlan& plan, PrivateResetPacket& packet)
{
    if (!packet.sceneResetOnce && !packet.temporal) return;
    if ((!plan.sceneResetOnce && !plan.temporal) || plan.rgbIdentity || plan.sceneResetWritten || !FSRD::PreFogSession::LateSrOnly())
        throw std::runtime_error("scene RESET requires the fixed late-SR-only route and a fresh admitted target");
    const char* error = nullptr;
    // The existing private RESET dispatch/compositor has already restored native
    // bindings. Its output is shader-readable on this SAME consumer recording;
    // the retained packet's mandatory pre-submit producer gate remains in force.
    auto work = FSRD::CyberpunkFogRgbWrite::Prepare(plan.device.Get(), packet.width, packet.height,
        plan.layers.privateReset[2].resource, plan.main, plan.originalView, plan.drawState.viewports[0],
        plan.drawState.scissors[0], &error);
    if (!work) throw std::runtime_error(error && *error ? error : "scene RESET RGB preparation refused");
    FogSceneResetEngineHost host { plan };
    FSRD::CyberpunkFogDenoiseAccess::Input input;
    input.image = authenticatedImage.load(); input.list = uintptr_t(list);
    input.originalPso = uintptr_t(plan.originalPso.Get()); input.originalFogScope = plan.depthSnapshot.scope.serial;
    input.depth = { plan.depthSnapshot.handle, plan.depthSnapshot.native };
    const auto result = FSRD::CyberpunkFogDenoiseAccess::RecordSceneRgb(host, input, [&] { return work->Record(list); });
    if (result.outcome == FSRD::CyberpunkFogDenoiseAccess::SceneOutcome::ScopeLostAfterMutation ||
        (result.bindingsRestored && !host.targetRestored)) PrivateResetFatal();
    if (result.outcome != FSRD::CyberpunkFogDenoiseAccess::SceneOutcome::SceneRecordedRestored ||
        !result.bindingsRestored || !host.targetRestored || !work->Recorded())
        throw std::runtime_error("scene RESET RGB recording/restoration refused; consumer must not submit");
    plan.sceneResetWritten = true;
    plan.layers.privateResetSceneWrite = true;
    if (plan.temporal && plan.captureFinal)
        plan.layers.privateOutputMode = FSRDFogLayerCapture::Layers::PrivateOutputMode::TemporalWindowFinalSceneControl32;
    { std::lock_guard lock(packet.mutex); packet.sceneRecorded = true; }
    const auto& source = plan.depthSnapshot.scope;
    plan.provenance[plan.temporal ? "temporal_scene_control" : "scene_reset_control"] = {
        { "mode", plan.temporal ? "temporal_window_32" : "scene_reset_once" }, { "status", "RGB_recorded_original_bindings_restored" },
        { "late_route", "fixed_SR_only" },
        { "temporal_history", plan.temporal ? "single_context_first_RESET_then_owned_previous" : "independent_one_shot_RESET_only" },
        { "source", plan.temporal ? "same_consumer_temporal_composed" : "same_consumer_private_RESET_composed" },
        { "source_address", uintptr_t(plan.layers.privateReset[2].resource.Get()) },
        { "target_address", uintptr_t(plan.main.Get()) }, { "scope", source.serial },
        { "list", source.list }, { "recording_generation", source.recordingGeneration },
        { "view", source.view }, { "frame_source_object", plan.depthFrameObject }, { "frame_source_value", plan.depthFrame },
        { "render_extent", { packet.width, packet.height } },
        { "alpha", "native_target_alpha_unwritten" }, { "bindings_restored", true },
        { "original_target_restored", true }, { "draw_recorded", true },
        { "submission", "existing_producer_and_complete_consumer_gate_required" },
        { "displayed_frame", "not_captured_by_this_readback" } };
}

void RecordPrivateReset(ID3D12GraphicsCommandList* list, CapturePlan& plan)
{
    auto* packet = plan.packet ? plan.packet : privateResetPacket.load(std::memory_order_acquire);
    if (!packet) return;
    struct RefuseIncomplete
    {
        PrivateResetPacket* packet;
        bool complete = false;
        ~RefuseIncomplete() { if (!complete) FailPacket(packet); }
    } attempted { packet };
    if (!plan.depthPrepared || !plan.layers.hardwareDepth.resource)
        throw std::runtime_error("private RESET requires successful native Fog hardware-depth copy");
    ClaimPrivateReset(*packet, plan.depthMetadata, plan.device.Get(), packet->fogClaimed);
    const auto source = packet->temporal ? ResetSource::ParseTemporal(plan.depthMetadata, packet->delta).current :
        ResetSource::Parse(plan.depthMetadata, packet->delta);
    auto c = source.parameters;
    FSRD::PrivateDenoise::FrameAdmission temporalAdmission;
    if (packet->temporal)
    {
        auto& window = *packet->temporal;
        std::optional<TemporalCamera::PreviousFrame> previous;
        {
            std::lock_guard lock(window.mutex);
            if (window.stopped || !window.policy || !window.queueIdentity ||
                window.policy->CommittedFrames() != packet->temporalKey.index ||
                (packet->temporalKey.index && !window.previous))
                throw std::runtime_error("temporal previous consumer has not returned/committed");
            previous = window.previous;
            temporalAdmission = { window.epoch, uintptr_t(window.queueIdentity.Get()), true };
        }
        const auto nativeReset = ResetSource::NativeReset(source);
        if ((packet->temporalKey.index && nativeReset) || !TemporalCamera::Build(source.rawSnapshot, source.motionScale,
            packet->delta, source.frame, nativeReset ? TemporalCamera::NativeReset::Requested : TemporalCamera::NativeReset::NotRequested,
            previous ? &*previous : nullptr, { window.epoch, true, previous.has_value() }, c))
            throw std::runtime_error("temporal current/prior camera or authored reset refused");
        // Software-epoch/view continuity is an explicit bounded experiment,
        // NOT an independently authenticated native allocation/world-origin epoch.
        plan.provenance["temporal_window"] = { { "epoch", window.epoch }, { "frame_count", 32 },
            { "frame_ordinal", packet->temporalKey.index + 1 }, { "frame", source.frame },
            { "final_capture", packet->temporalKey.CaptureFinal() },
            { "clock", "selected_original_Fog_draw_CPU_interval_not_native_simulation_duration" },
            { "previous_timestamp_ms", packet->previousFogTimestamp }, { "timestamp_ms", packet->fogTimestamp },
            { "view_continuity", "experimental_software_epoch_same_source_route_and_native_history_byte" },
            { "native_world_origin_epoch_proven", false }, { "submission_ledger", window.ledgerRelative } };
    }
    FSRD::PrivateDenoise::Parameters parameters;
    auto& conversion = parameters.conversion;
    const auto& rays = packet->rays->Outputs();
    const auto& guides = packet->guides->Outputs();
    conversion.Resources.InColor = plan.layers.before.resource.Get();
    conversion.Resources.InDepth = plan.layers.hardwareDepth.resource.Get();
    conversion.Resources.InMotionVectors = rays[0].Get();
    conversion.Resources.InNormals = guides[2].Get();
    conversion.Resources.InRoughness = nullptr;
    conversion.Resources.InSpecHitDist = rays[1].Get();
    conversion.Resources.InDiffAlbedo = guides[0].Get();
    conversion.Resources.InSpecAlbedo = guides[1].Get();
    std::memcpy(&conversion.InvViewMatrix, c.inverseView.data(), sizeof(conversion.InvViewMatrix));
    std::memcpy(&conversion.InvProjMatrix, c.inverseProjection.data(), sizeof(conversion.InvProjMatrix));
    std::memcpy(&conversion.PrevViewMatrix, c.previousView.data(), sizeof(conversion.PrevViewMatrix));
    std::memcpy(&conversion.PreviousDepthProjection, c.previousDepthProjection.data(), sizeof(conversion.PreviousDepthProjection));
    std::memcpy(&conversion.RenderSize, c.renderSize.data(), sizeof(conversion.RenderSize));
    conversion.NearPlane = c.nearPlane; conversion.FarPlane = c.farPlane; conversion.Flags = c.conversionFlags;
    auto& d = parameters.dispatch;
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    d.renderSize = { source.width, source.height };
    d.cameraRight = { c.cameraRight[0], c.cameraRight[1], c.cameraRight[2] };
    d.cameraUp = { c.cameraUp[0], c.cameraUp[1], c.cameraUp[2] };
    d.cameraForward = { c.cameraForward[0], c.cameraForward[1], c.cameraForward[2] };
    d.cameraPositionDelta = { c.cameraPositionDelta[0], c.cameraPositionDelta[1], c.cameraPositionDelta[2] };
    d.motionVectorScale = { c.motionScale[0], c.motionScale[1], c.motionScale[2] };
    d.jitterOffsets = { c.jitterNdc[0], c.jitterNdc[1] };
    d.cameraAspectRatio = c.aspectRatio; d.cameraNear = c.nearPlane; d.cameraFar = c.farPlane;
    d.cameraFovAngleVertical = c.verticalFovRadians; d.deltaTime = c.deltaMilliseconds;
    d.frameIndex = c.frameIndex; d.flags = c.dispatchFlags;
    parameters.maxRenderSize = d.renderSize;
    parameters.providerId = packet->provider; parameters.settings = packet->settings;
    const char* error = nullptr;
    auto work = packet->temporal
        ? FSRD::PrivateDenoise::PrepareFrame(packet->temporal->session, parameters, temporalAdmission, &error)
        : FSRD::PrivateDenoise::Prepare(plan.device.Get(), parameters, &error);
    if (!work) throw std::runtime_error(error && *error ? error : "private RESET preparation failed");
    const auto repeated = ResetSource::Parse(Json::parse(FSRDCyberpunkEarlyGuides::Describe(
        scope->context, authenticatedImage.load())), packet->delta);
    if (!source.SameFrame(repeated))
        throw std::runtime_error("private RESET current camera changed during provider preparation");
    // This owner must NOT become part of plan's submission-ticket ownership tree.
    const auto firstRead = PrivateResetPoint(plan.endpoint->list.Get(), plan.drawState.generation);
    {
        std::lock_guard lock(packet->mutex);
        packet->denoise = work;
        packet->consumerIdentity = plan.endpoint->list;
        packet->consumer = { uintptr_t(plan.endpoint->list.Get()), plan.drawState.generation };
        if (!packet->policy.EmbedConsumer(packet->consumer, firstRead, true))
            throw std::runtime_error("private RESET consumer obligation refused");
    }
    // Only our own pre-Fog snapshot changes state. The original target/alpha
    // stays untouched. Its producer may RECORD later on CPU, but the mandatory
    // pre-submit gate admits reads only after its exact GPU dependency is proved.
    constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Transition(list, plan.layers.before.resource.Get(), plan.layers.before.state, readable);
    plan.layers.before.state = readable;
    FogDepthEngineHost host { plan };
    FSRD::CyberpunkFogDenoiseAccess::Input input;
    input.image = authenticatedImage.load(); input.list = uintptr_t(list);
    input.originalPso = uintptr_t(plan.originalPso.Get()); input.originalFogScope = plan.depthSnapshot.scope.serial;
    input.depth = { plan.depthSnapshot.handle, plan.depthSnapshot.native };
    const auto result = FSRD::CyberpunkFogDenoiseAccess::RecordPrivateCompute(host, input, [&] { return work->Record(list); });
    if (result.outcome == FSRD::CyberpunkFogDenoiseAccess::Outcome::ScopeLostAfterMutation)
        PrivateResetFatal();
    if (result.outcome != FSRD::CyberpunkFogDenoiseAccess::Outcome::PrivateRecordedRestored || !work->Recorded())
        throw std::runtime_error("private RESET recording/restoration failed");
    const auto& outputs = work->Outputs();
    plan.layers.privateReset = {
        FSRDFogLayerCapture::Texture { outputs.radiance, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, readable },
        FSRDFogLayerCapture::Texture { outputs.denoised, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, readable },
        FSRDFogLayerCapture::Texture { outputs.composed, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, readable } };
    const auto& effective = work->EffectiveParameters();
    const auto& ec = effective.conversion;
    const auto& ed = effective.dispatch;
    std::array<uint32_t, 60> constantWords {};
    std::memcpy(constantWords.data(), &ec.InvViewMatrix, 64);
    std::memcpy(constantWords.data() + 16, &ec.InvProjMatrix, 64);
    std::memcpy(constantWords.data() + 32, &ec.PrevViewMatrix, 64);
    std::memcpy(constantWords.data() + 48, &ec.PreviousDepthProjection, 16);
    std::memcpy(constantWords.data() + 52, &ec.RenderSize, 16);
    constantWords[56] = std::bit_cast<uint32_t>(ec.NearPlane);
    constantWords[57] = std::bit_cast<uint32_t>(ec.FarPlane);
    constantWords[58] = ec.Flags;
    const auto floatWords = [](std::initializer_list<float> values) {
        auto words = Json::array();
        for (float value : values) words.push_back(std::bit_cast<uint32_t>(value));
        return words;
    };
    const char* denoiseEvidence = plan.temporal ? "temporal_denoise" : "private_reset";
    plan.provenance[denoiseEvidence] = {
        { "status", "private_commands_recorded_submission_gate_required" }, { "scene_modified", false },
        { "provider_id", packet->provider }, { "provider_name", work->ProviderName() },
        { "delta_ms", packet->delta }, { "delta_source", plan.temporal ?
            "selected_original_Fog_draw_CPU_interval_not_native_simulation_duration" : "explicit_reset_control_not_captured_duration" },
        { "conversion_flags", ec.Flags }, { "dispatch_flags", ed.flags },
        { "effective_conversion_cb_words", constantWords },
        { "effective_dispatch_float_words", {
            { "motion_scale", floatWords({ ed.motionVectorScale.x, ed.motionVectorScale.y, ed.motionVectorScale.z }) },
            { "jitter", floatWords({ ed.jitterOffsets.x, ed.jitterOffsets.y }) },
            { "camera_right", floatWords({ ed.cameraRight.x, ed.cameraRight.y, ed.cameraRight.z }) },
            { "camera_up", floatWords({ ed.cameraUp.x, ed.cameraUp.y, ed.cameraUp.z }) },
            { "camera_forward", floatWords({ ed.cameraForward.x, ed.cameraForward.y, ed.cameraForward.z }) },
            { "camera_delta", floatWords({ ed.cameraPositionDelta.x, ed.cameraPositionDelta.y, ed.cameraPositionDelta.z }) },
            { "aspect_near_far_fov_delta", floatWords({ ed.cameraAspectRatio, ed.cameraNear, ed.cameraFar,
                                                        ed.cameraFovAngleVertical, ed.deltaTime }) } } },
        { "frame", source.frame }, { "view", source.view }, { "frame_object", source.object },
        { "settings", { effective.settings.crossBilateralNormalStrength, effective.settings.stabilityBias,
            effective.settings.maxRadiance, effective.settings.radianceClipStdK,
            effective.settings.gaussianKernelRelaxation, effective.settings.disocclusionThreshold } },
        { "camera", source.camera }, { "bindings_restored", result.bindingsRestored },
        { "first_read_ordinal", firstRead }, { "temporal_history", plan.temporal ?
            "single context; first RESET only; previous committed after native consumer return" : "independent one-shot RESET only" } };
    // Seal only after subsequent original/private draw and readback recording in
    // FinishCapture. Exceptions anywhere before that leave the gate unsealed.
    RecordSceneReset(list, plan, *packet);
    plan.provenance[denoiseEvidence]["scene_modified"] = plan.sceneResetWritten;
    attempted.complete = true;
}

std::shared_ptr<CapturePlan> PrepareCapture(ID3D12GraphicsCommandList* list, UINT count, UINT instances,
                                           UINT start, UINT firstInstance, uintptr_t nativeCaller,
                                           PrivateResetPacket* selectedTemporal = nullptr)
{
    const bool temporal = selectedTemporal && selectedTemporal->temporal;
    if (TemporalRecordingRequested() && !temporal) return {};
    if (privateResetArming.load(std::memory_order_acquire)) return {};
    if (!captureEnabled.load() || !captureTrackingValid.load() || !scope ||
        (!temporal && (captureStarted.load() || !FSRDFogLayerCapture::WantsCapture())))
        return {};
    // Recheck AFTER acquiring the capture slot's synchronization: arming may
    // have started between the first latch read and WantsCapture observing the
    // newly queued slot. False here publishes the entire immutable packet.
    if (privateResetArming.load(std::memory_order_acquire)) return {};
    const auto& s = *scope;
    if (count != 3 || instances != 1 || start != 0 || firstInstance != 0 ||
        list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || s.psoList != list || !s.pso ||
        s.rtvList != list || s.rtvCount != 1 || s.hasDsv)
    {
        RefuseCapture("draw, in-scope PSO, or exact single RTV not observed");
        return {};
    }
    auto plan = std::make_shared<CapturePlan>();
    plan->packet = selectedTemporal;
    plan->temporal = temporal;
    plan->captureFinal = temporal && selectedTemporal->temporalKey.CaptureFinal();
    plan->charge = temporal ? selectedTemporal->charge : nullptr;
    const auto* rgbPacket = rgbIdentityPacket.load(std::memory_order_acquire);
    plan->rgbIdentity = rgbPacket != nullptr;
    const auto* resetPacket = selectedTemporal ? selectedTemporal : privateResetPacket.load(std::memory_order_acquire);
    plan->sceneResetOnce = resetPacket && resetPacket->sceneResetOnce;
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
    const UINT writeWidth = rgbPacket ? rgbPacket->width : (resetPacket ? resetPacket->width : 0);
    const UINT writeHeight = rgbPacket ? rgbPacket->height : (resetPacket ? resetPacket->height : 0);
    if ((plan->rgbIdentity || plan->sceneResetOnce || plan->temporal) &&
        ((plan->rgbIdentity && (resetPacket || lightingRequested.load() || earlyRequested.load())) ||
         ((plan->sceneResetOnce || plan->temporal) && (rgbPacket || !FSRD::PreFogSession::LateSrOnly())) ||
         !s.fogHelper || nativeCaller != authenticatedImage.load() + FSRD::CyberpunkFogDepth::DrawReturnRva ||
         desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.MipLevels != 1 ||
         desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
         desc.Width != writeWidth || desc.Height != writeHeight ||
         !IsFullRgbViewport(state, writeWidth, writeHeight)))
    {
        RefuseCapture("pre-Fog RGB write requires exact full typed RGBA16F original draw and exclusive mode");
        return {};
    }
    Json earlyAvailability;
    if (lightingRequested.load())
    {
        Json source;
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            source = data.lightingCaptureSource;
        }
        // Independent CPU recording can reach this frame's Fog draw before
        // lighting publishes its receipt. Never skip that draw waiting for
        // candidate metadata; completed captures must be paired independently.
        if (!source.is_null())
        {
            source["pairing_authority"] = "candidate metadata only; current frame/camera match not asserted";
            plan->provenance["lighting_recording_candidate"] = std::move(source);
        }
    }
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
    if (!temporal && captureStarted.exchange(true))
        return {};
    if (FAILED(plan->main->GetDevice(IID_PPV_ARGS(&plan->device))))
        throw std::runtime_error("fog target device unavailable");
    ComPtr<ID3D12Device> listDevice;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice))) || listDevice.Get() != plan->device.Get())
        throw std::runtime_error("fog target/list device mismatch");
    if (rgbPacket)
    {
        ComPtr<IUnknown> identity;
        if (FAILED(plan->device->QueryInterface(IID_PPV_ARGS(&identity))) || !identity ||
            identity.Get() != rgbPacket->deviceIdentity.Get())
            throw std::runtime_error("RGB identity armed device differs from original Fog target");
    }

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
    const bool diskCapture = !temporal || plan->captureFinal;
    const auto authoredBytes = diskCapture ? plan->device->GetResourceAllocationInfo(0, 1, &authoredDesc).SizeInBytes : 0;
    const UINT64 copyCount = plan->rgbIdentity ? 3 : (diskCapture ? 2 : 1);
    if (mainBytes > MaxCaptureTextureBytes || copyBytes > MaxCaptureTextureBytes || authoredBytes > MaxCaptureTextureBytes ||
        mainBytes + copyCount * copyBytes + authoredBytes > MaxCaptureTextureBytes)
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
    if (diskCapture)
    {
        allocate(beforeDesc, D3D12_RESOURCE_STATE_COPY_DEST, plan->layers.after);
        allocate(authoredDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, plan->layers.authored);
    }
    if (plan->rgbIdentity)
    {
        allocate(beforeDesc, D3D12_RESOURCE_STATE_COPY_DEST, plan->layers.rgbIdentity);
        plan->rgbIdentityPrepared = true;
        plan->provenance["rgb_identity_control"] = {
            { "mode", "rgb_identity_only" }, { "status", "private_snapshot_prepared" },
            { "scene_write", "RGB only from same-list pre-Fog copy; native alpha unwritten" },
            { "denoising", "unchanged ordinary late RR; no early guide/RESET work" },
            { "identity_passed", nullptr }, { "topology", "authenticated original preparation and engine+0x628==4" },
            { "coordinates", "sample-interpolated UV; actual compiled sample-frequency reflection required" }
        };
    }
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
    if (diskCapture) plan->device->CreateRenderTargetView(plan->layers.authored.resource.Get(), &view, plan->authoredRtv);
    plan->device->CreateRenderTargetView(plan->main.Get(), &plan->originalView, plan->frozenOriginalRtv);
    plan->provenance["restoration"] = { { "frozen_rtv_handle", plan->frozenOriginalRtv.ptr },
        { "binding", "exact owned original resource/view; original CPU descriptor may have been reused" } };
    if (diskCapture) PrepareBoundCb12Target(*plan, MaxCaptureTextureBytes - (mainBytes + copyCount * copyBytes + authoredBytes));
    plan->retainedTextureBytes = mainBytes + copyCount * copyBytes + authoredBytes;
    if (plan->layers.boundCb12.resource)
    {
        const auto cbDesc = plan->layers.boundCb12.resource->GetDesc();
        plan->retainedTextureBytes += plan->device->GetResourceAllocationInfo(0, 1, &cbDesc).SizeInBytes;
    }
    PrepareFogDepth(*plan, nativeCaller);
    if (plan->charge) plan->charge->Add(plan->retainedTextureBytes);

    // Retain BEFORE the first private-copy command. This independent ticket keeps
    // earlier work alive even if the later readback helper refuses or throws.
    auto finalTicket = FSRDSubmission::Retain(plan->device.Get(), list, plan);
    if (!finalTicket)
        throw std::runtime_error("Fog capture lifetime retention unavailable");
    if (temporal)
    {
        std::lock_guard lock(selectedTemporal->mutex);
        selectedTemporal->finalTicket = std::move(finalTicket); // Controller owns ticket; ticket never owns controller/Work.
    }
    CopyMain(list, *plan, plan->layers.before.resource.Get());
    RecordFogDepth(list, *plan); // Independent private snapshot on Fog's own list, no guide reads.
    if (plan->fatalEarlyRecording) return plan;
    if (plan->rgbIdentity)
        RecordRgbIdentity(list, *plan); // Explicit identity-only marker; no denoised replacement or late-route change.
    else
    {
        RecordPrivateReset(list, *plan);
        PrepareAndRecordEarlyGuides(list, *plan); // Optional private outputs; never writes the original scene.
    }
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
    auto* selected = plan->packet ? plan->packet : privateResetPacket.load(std::memory_order_acquire);
    if (plan->temporal && !plan->captureFinal)
    {
        // The original Fog draw has already run exactly once. Intermediate
        // frames seal actual scene work, not an unrequested disk capture.
        if (!selected || !plan->sceneResetWritten || !plan->layers.privateReset[0].resource)
            throw std::runtime_error("temporal intermediate scene recording incomplete");
        const auto point = PrivateResetPoint(plan->endpoint->list.Get(), plan->drawState.generation);
        std::lock_guard lock(selected->mutex);
        if (!selected->policy.SealConsumer({ uintptr_t(plan->endpoint->list.Get()), plan->drawState.generation },
                                           point, true, true))
            throw std::runtime_error("temporal intermediate consumer seal refused");
        selected->consumerSealed = true;
        return;
    }
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
    if (plan->layers.privateReset[0].resource)
    {
        auto* packet = selected;
        const auto point = PrivateResetPoint(plan->endpoint->list.Get(), plan->drawState.generation);
        std::lock_guard lock(packet->mutex);
        if (!recorded || ((packet->sceneResetOnce || packet->temporal) && !plan->sceneResetWritten) || !packet->policy.SealConsumer(
            { uintptr_t(plan->endpoint->list.Get()), plan->drawState.generation }, point, true, true))
            throw std::runtime_error("private RESET completed consumer seal refused");
        packet->consumerSealed = true;
    }
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

PrivateResetPacket* ObserveTemporalFog(ID3D12GraphicsCommandList* list, UINT count, UINT instances,
                                      UINT start, UINT firstInstance, uintptr_t caller, double timestamp)
{
    auto* window = temporalWindow.load(std::memory_order_acquire);
    if (!window || privateResetArming.load(std::memory_order_acquire) || !scope || !scope->fogHelper ||
        caller != authenticatedImage.load() + FSRD::CyberpunkFogDepth::DrawReturnRva ||
        count != 3 || instances != 1 || start || firstInstance) return nullptr;
    if (window->stopped) return nullptr; // Missing producer roles may still drain through their own selectors.
    {
        std::lock_guard lock(window->mutex);
        // Completed windows keep their submission evidence, but no longer
        // collect fresh Fog bindings. Do not diagnose ordinary later frames as
        // missing inputs for a test whose last consumer has already returned.
        if (window->policy && window->policy->Complete()) return nullptr;
    }
    const auto& s = *scope;
    if (!s.boundRtv.known)
    {
        // Publication can fall between an original OM bind and this draw. The
        // first complete original-use snapshot begins warm-up; never AddRef a
        // stale descriptor just to make an in-flight partial scope eligible.
        std::lock_guard lock(window->mutex);
        if (!window->policy) return nullptr;
    }
    if (!captureTrackingValid.load() || !list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        s.psoList != list || !s.pso || s.rtvList != list || s.rtvCount != 1 || s.hasDsv ||
        !s.boundRtv.known || !s.boundRtv.resource ||
        s.boundRtv.view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        throw std::runtime_error("temporal selected Fog original-use observation incomplete");
    const auto desc = s.boundRtv.resource->GetDesc();
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Width != window->width ||
        desc.Height != window->height || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || !MatchLiveCode(authenticatedImage.load(), FogTopologyCode))
        throw std::runtime_error("temporal selected Fog target changed");
    uintptr_t tls = 0, engine = 0, nativeList = 0;
    uint8_t initialized = 0;
    uint32_t topology = 0;
    if (!ReadEarly(uintptr_t(__readgsqword(0x58)), tls) || !ReadEarlyAt(tls, 0x14, initialized) || !initialized ||
        !ReadEarlyAt(tls, 0x188, engine) || !ReadEarlyAt(engine, 0x30, nativeList) || nativeList != uintptr_t(list) ||
        !ReadEarlyAt(engine, 0x628, topology) || topology != 4)
        throw std::runtime_error("temporal selected Fog native context/topology unavailable");
    auto identity = ListIdentity(list);
    ResetPolicy::Recording recording {};
    {
        auto& data = Data(); std::lock_guard lock(data.mutex);
        const auto state = data.lists.find(identity.Get());
        const auto shader = std::find_if(data.tagged.begin(), data.tagged.end(),
                                         [&](const auto& item) { return item.pso.Get() == s.pso && item.authored; });
        if (state == data.lists.end() || !state->second.known || state->second.predicated ||
            state->second.renderPass || state->second.queryCount || shader == data.tagged.end() ||
            !IsFullRgbViewport(state->second, window->width, window->height))
            throw std::runtime_error("temporal selected Fog list history or authored PSO unavailable");
        recording = { uintptr_t(identity.Get()), state->second.generation };
    }
    // Installs only the original submission observer. Its temporary discovery
    // queue is NOT the window queue; that is obtained from an actual return.
    auto native = ResTrack_Dx12::PrepareSubmission(window->device.Get(), list);
    ComPtr<IUnknown> observed;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&observed))) || observed.Get() != identity.Get())
        throw std::runtime_error("temporal warm-up native list identity unavailable");
    const auto metadata = Json::parse(FSRDCyberpunkEarlyGuides::Describe(s.context, authenticatedImage.load()));
    const auto raw = ResetSource::ParseRawTemporal(metadata);
    double previousTime = 0;
    bool ready = false;
    {
        std::lock_guard lock(window->mutex);
        if (window->policy && window->policy->Complete()) return nullptr;
        if (window->lastFog && (window->lastFog->view != raw.current.view || window->lastFog->object != raw.current.object ||
            window->lastFog->frame == UINT32_MAX || raw.current.frame != window->lastFog->frame + 1))
            throw std::runtime_error("temporal selected Fog source cadence or view changed");
        previousTime = window->lastFogTimestamp;
        ready = window->warmupReturned && window->lastFog.has_value();
    }
    auto* packet = ready ? SelectTemporalFrame(*window, metadata, window->device.Get(), WindowPolicy::Role::Fog) : nullptr;
    const double delta = timestamp - previousTime;
    if (packet)
    {
        // Positive finite binary representation; /fp:fast must not erase NaN guards.
        const float measured = static_cast<float>(delta);
        if ((std::bit_cast<uint64_t>(timestamp) & 0x7ff0000000000000ull) == 0x7ff0000000000000ull ||
            (std::bit_cast<uint32_t>(measured) & 0x7f800000u) == 0x7f800000u || measured <= 0)
            throw std::runtime_error("temporal selected Fog CPU interval refused");
        auto timed = ResetSource::ParseTemporal(metadata, measured);
        std::lock_guard lock(packet->mutex);
        packet->delta = measured; packet->fogTimestamp = timestamp; packet->previousFogTimestamp = previousTime;
        packet->timedSource = std::move(timed);
    }
    {
        std::lock_guard lock(window->mutex);
        if (!window->warmupRecording.list)
        { window->warmupRecording = recording; window->warmupList = identity; }
        window->lastFog = raw.current; window->lastFogTimestamp = timestamp;
    }
    return packet;
}

void WINAPI HookDraw(ID3D12GraphicsCommandList* list, UINT count, UINT instances, UINT start, UINT firstInstance)
{
    const auto nativeCaller = uintptr_t(_ReturnAddress());
    const auto temporalTimestamp = TemporalRecordingRequested() ? Util::MillisecondsNow() : 0.0;
    LogDraw(list, false, count, instances, start, 0, firstInstance);
    std::shared_ptr<LightingCapturePlan> lightingPlan;
    if (captureEnabled.load() && !inMetadata && !privateResetArming.load(std::memory_order_acquire) &&
        lightingRequested.load() && !lightingAttempted.load() &&
        (TemporalRecordingRequested() || FSRDFogLayerCapture::WantsEarlyGuideCapture()) &&
        MatchesFinalLightingDraw(lightingScope != nullptr, lightingScope && lightingScope->finalHelper,
            nativeCaller, authenticatedImage.load(), count, instances, start, firstInstance) &&
        (TemporalRecordingRequested() || !lightingAttempted.exchange(true)))
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
                if (diagnostic && diagnostic->packet) FailPacket(diagnostic->packet);
                else if (auto* window = temporalWindow.load()) StopTemporalWindow(*window, error.what());
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
            try {
                auto* selected = ObserveTemporalFog(list, count, instances, start, firstInstance, nativeCaller, temporalTimestamp);
                plan = PrepareCapture(list, count, instances, start, firstInstance, nativeCaller, selected);
                if (selected && !plan) FailPacket(selected);
            }
            catch (const std::exception& error)
            {
                if (auto* window = temporalWindow.load()) StopTemporalWindow(*window, error.what());
                else captureStarted.store(true);
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
                if (lightingPlan->packet) FailPacket(lightingPlan->packet);
                FSRDFogLayerCapture::CancelEarlyGuideRequest();
                lightingPlan->provenance["status"] = "refused_after_original_draw";
                lightingPlan->provenance["reason"] = error.what();
                LOG_WARN("[FSRRR lighting guides] refusal {}", lightingPlan->provenance.dump());
            }
        });
    if (plan)
    {
        if (!plan->temporal || plan->captureFinal)
            PublishFogEndpoint(plan); // Publish only AFTER the original target draw was recorded once.
        Metadata([&] {
            try { FinishCapture(list, plan); }
            catch (const std::exception& error)
            {
                if (plan->packet) FailPacket(plan->packet);
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
    if (captureEnabled.load())
        data.lightingShaders.Record(pso, desc.VS, desc.PS,
            std::string_view(path) == "CreatePipelineState" ? FSRD::CyberpunkLightingShaders::CreationPath::Stream
                                                          : FSRD::CyberpunkLightingShaders::CreationPath::Graphics,
            [&](const void* pointer, size_t bytes) -> std::string {
                if (!pointer || !bytes || bytes > 65536) return {};
                std::vector<std::byte> snapshot(bytes);
                if (!ReadExactMemory(uintptr_t(pointer), snapshot.data(), bytes)) return {};
                Hash hash(data.crypto);
                hash.Add(snapshot.data(), ULONG(bytes));
                return hash.Finish();
            });
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

void ArmRgbIdentity(ID3D12Device* device, UINT width, UINT height) noexcept
{
    if (!active.load() || !captureEnabled.load() || !captureTrackingValid.load() ||
        rgbIdentityPacket.load(std::memory_order_acquire) || privateResetPacket.load(std::memory_order_acquire) || TemporalRecordingRequested()) return;
    static std::atomic<ULONGLONG> nextPoll { 0 };
    auto due = nextPoll.load();
    const auto now = GetTickCount64();
    if (now < due || !nextPoll.compare_exchange_strong(due, now + 1000)) return;
    Metadata([&] {
        std::unique_lock requestLock(captureRequestMutex, std::try_to_lock);
        if (!requestLock) return;
        const auto path = Util::ExePath().parent_path() / L"FSRRR-prefog-rgb-identity.request";
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        if (privateResetArming.exchange(true, std::memory_order_acq_rel)) return;
        struct FinishArming
        {
            ~FinishArming() { privateResetArming.store(false, std::memory_order_release); }
        } finishArming;
        if (rgbIdentityPacket.load(std::memory_order_acquire) || privateResetPacket.load(std::memory_order_acquire) || TemporalRecordingRequested()) return;
        bool queued = false;
        try
        {
            const auto fog = FSRDFogLayerCapture::GetStatus();
            const auto guides = FSRDFogLayerCapture::GetEarlyGuideStatus();
            if (!device || !width || !height || width > 8192 || height > 8192 ||
                captureStarted.load() || lightingRequested.load() || lightingAttempted.load() ||
                earlyRequested.load() || earlyAttempted.load() || rayCopyAttempted.load() ||
                fog.queued || fog.busy || fog.attempted || guides.queued || guides.busy || guides.attempted ||
                !fogDepthAuthenticated.load() || authenticatedImage.load() != uintptr_t(GetModuleHandleW(nullptr)) ||
                !MatchLiveCode(authenticatedImage.load(), FogTopologyCode) || std::filesystem::file_size(path) > 4096)
                throw std::runtime_error("RGB identity requires an unused authenticated capture session");
            Json controls;
            {
                // Close the Windows reader before DeleteFileW; no open handle
                // without DELETE sharing may survive marker consumption.
                std::ifstream file(path, std::ios::binary);
                file >> controls;
            }
            if (!controls.is_object() || controls.size() != 1 || controls.at("mode") != "rgb_identity_only")
                throw std::runtime_error("RGB identity requires exactly the explicit rgb_identity_only mode");
            auto packet = std::make_unique<RgbIdentityPacket>();
            packet->device = device; packet->width = width; packet->height = height;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&packet->deviceIdentity))) || !packet->deviceIdentity)
                throw std::runtime_error("RGB identity device identity unavailable");
            if (!FSRDFogLayerCapture::Request())
                throw std::runtime_error("RGB identity Fog capture slot unavailable");
            queued = true;
            if (!DeleteFileW(path.c_str()))
                throw std::runtime_error(std::format("RGB identity marker could not be consumed (Win32 {})", GetLastError()));
            rgbIdentityPacket.store(packet.release(), std::memory_order_release);
            queued = false;
            LOG_INFO("[FSRRR RGB identity] armed {}x{} one-shot RGB-only control; alpha unwritten, no guide/RESET or late-route change", width, height);
        }
        catch (const std::exception& error)
        {
            if (queued) FSRDFogLayerCapture::CancelRequest();
            LOG_WARN("[FSRRR RGB identity] arm refused: {}", error.what());
        }
        catch (...)
        {
            if (queued) FSRDFogLayerCapture::CancelRequest();
            throw; // Metadata contains logging/allocation failures; latch still clears.
        }
    });
}

void ArmPrivateReset(ID3D12Device* device, UINT width, UINT height) noexcept
{
    if (!active.load() || !captureEnabled.load() || !captureTrackingValid.load() ||
        privateResetPacket.load(std::memory_order_acquire) || rgbIdentityPacket.load(std::memory_order_acquire) || TemporalRecordingRequested()) return;
    static std::atomic<ULONGLONG> nextPoll { 0 };
    auto due = nextPoll.load();
    const auto now = GetTickCount64();
    if (now < due || !nextPoll.compare_exchange_strong(due, now + 1000)) return;
    Metadata([&] {
        std::unique_lock requestLock(captureRequestMutex, std::try_to_lock);
        if (!requestLock) return;
        const auto path = Util::ExePath().parent_path() / L"FSRRR-prefog-reset.request";
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        if (privateResetArming.exchange(true, std::memory_order_acq_rel)) return;
        struct FinishArming
        {
            ~FinishArming() { privateResetArming.store(false, std::memory_order_release); }
        } finishArming;
        if (privateResetPacket.load(std::memory_order_acquire) || rgbIdentityPacket.load(std::memory_order_acquire) || TemporalRecordingRequested()) return;
        try
        {
            if (!device || !width || !height || width > 8192 || height > 8192 ||
                captureStarted.load() || lightingRequested.load() || lightingAttempted.load() || earlyRequested.load() ||
                rayCopyAttempted.load() || !rayBindingsAuthenticated.load() || !fogDepthAuthenticated.load() ||
                authenticatedImage.load() != uintptr_t(GetModuleHandleW(nullptr)) || std::filesystem::file_size(path) > 4096)
                throw std::runtime_error("private RESET requires an unused authenticated capture session");
            Json controls;
            {
                // Windows file streams do not share DELETE access. Close the
                // reader before consuming this request with DeleteFileW below.
                std::ifstream file(path, std::ios::binary);
                file >> controls;
            }
            const bool sceneReset = controls.at("mode") == "scene_reset_once";
            if ((controls.at("mode") != "private_reset_only" && !sceneReset) ||
                controls.at("delta_source") != "explicit_reset_control_not_captured_duration")
                throw std::runtime_error("RESET requires explicit private-only or one-shot scene experiment controls");
            if (sceneReset && (!FSRD::PreFogSession::LateSrOnly() ||
                !MatchLiveCode(authenticatedImage.load(), FogTopologyCode)))
                throw std::runtime_error("scene RESET requires restart-fixed late-SR-only mode and authenticated RGB target route");
            ComPtr<IUnknown> identity;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))))
                throw std::runtime_error("private RESET device identity unavailable");
            auto packet = std::make_unique<PrivateResetPacket>(uintptr_t(identity.Get()));
            packet->device = device; packet->deviceIdentity = identity;
            packet->sceneResetOnce = sceneReset;
            packet->width = width; packet->height = height;
            packet->provider = controls.at("provider_id").get<uint64_t>();
            packet->delta = controls.at("delta_ms").get<float>();
            const auto& settings = controls.at("settings");
            if (!packet->provider || settings.size() != 6 || !std::isfinite(packet->delta) || packet->delta <= 0)
                throw std::runtime_error("private RESET provider/duration/settings incomplete");
            packet->settings = { settings.at("1").get<float>(), settings.at("2").get<float>(),
                settings.at("3").get<float>(), settings.at("4").get<float>(),
                settings.at("5").get<float>(), settings.at("6").get<float>() };
            const char* error = nullptr;
            packet->guides = FSRD::CyberpunkGuidePass::AllocateTargets(device, width, height, &error);
            packet->rays = FSRD::PrivateRayCopy::AllocateTargets(device, width, height, &error);
            if (!packet->guides || !packet->rays)
                throw std::runtime_error(error && *error ? error : "private RESET target allocation failed");
            UINT64 bytes = 0;
            const auto account = [&](const auto& targets) {
                for (const auto& target : targets)
                {
                    const auto desc = target->GetDesc();
                    const auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
                    if (!size || size > MaxCaptureTextureBytes - bytes)
                        throw std::runtime_error("private RESET combined target budget exceeded");
                    bytes += size;
                }
            };
            account(packet->guides->Outputs()); account(packet->rays->Outputs());
            if (!FSRDFogLayerCapture::RequestEarlyGuides())
                throw std::runtime_error("private RESET guide capture slot unavailable");
            if (!FSRDFogLayerCapture::Request())
            {
                FSRDFogLayerCapture::CancelEarlyGuideRequest();
                throw std::runtime_error("private RESET Fog capture slot unavailable");
            }
            if (!DeleteFileW(path.c_str()))
            {
                const auto error = GetLastError();
                FSRDFogLayerCapture::CancelEarlyGuideRequest(); FSRDFogLayerCapture::CancelRequest();
                throw std::runtime_error(std::format("private RESET marker could not be consumed (Win32 {})", error));
            }
            privateResetPacket.store(packet.release(), std::memory_order_release);
            lightingRequestedAt.store(GetTickCount64());
            lightingRequested.store(true);
            LOG_INFO("[FSRRR private RESET] armed {}x{} fixed private targets; mode={}; exact frame and pre-submit gate required",
                     width, height, sceneReset ? "scene_reset_once_RGB_only_fixed_late_SR" : "private_only_no_game_writes");
        }
        catch (const std::exception& error) { LOG_WARN("[FSRRR private RESET] arm refused: {}", error.what()); }
    });
}

namespace
{
constexpr uint64_t TemporalToken = 1ull << 63, WarmupToken = 1ull << 62;

TemporalTargets AllocateTemporalTargets(TemporalWindow& window)
{
    TemporalTargets result;
    result.charge = std::make_shared<TemporalCharge>(window.budget);
    const char* error = nullptr;
    result.guides = FSRD::CyberpunkGuidePass::AllocateTargets(window.device.Get(), window.width, window.height, &error);
    result.rays = FSRD::PrivateRayCopy::AllocateTargets(window.device.Get(), window.width, window.height, &error);
    if (!result.guides || !result.rays) throw std::runtime_error(error && *error ? error : "temporal target allocation refused");
    const auto account = [&](const auto& textures) {
        for (const auto& texture : textures)
        {
            const auto desc = texture->GetDesc();
            result.charge->Add(window.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes);
        }
    };
    account(result.guides->Outputs()); account(result.rays->Outputs());
    return result;
}

void MaintainTemporalWindow(TemporalWindow& window)
{
    // No waits and no provider/native calls under the controller lock. Small
    // immutable policy tombstones stay; only fence-complete heavy owners retire.
    std::array<PrivateResetPacket*, 32> frames {};
    {
        std::lock_guard lock(window.mutex);
        for (size_t i = 0; i < frames.size(); ++i) frames[i] = window.frames[i].get();
    }
    for (auto* frame : frames)
    {
        if (!frame) continue;
        std::shared_ptr<FSRDSubmission::Ticket> ticket;
        {
            std::lock_guard lock(frame->mutex);
            if (frame->retired || !frame->returned) continue;
            ticket = frame->finalTicket;
        }
        if (!ticket || !FSRDSubmission::Complete(ticket)) continue;
        TemporalTargets targets;
        std::shared_ptr<FSRD::PrivateDenoise::Work> work;
        std::shared_ptr<RayCopyBundle> rays;
        {
            std::lock_guard lock(frame->mutex);
            if (frame->retired) continue;
            frame->retired = true;
            targets = { std::move(frame->guides), std::move(frame->rays), std::move(frame->charge) };
            work = std::move(frame->denoise); rays = std::move(frame->rayCopy);
            frame->finalTicket.reset(); // ticket local keeps COM-backed storage out of this lock.
        }
        // Locals release here, outside controller/frame locks. A ticket-owned
        // leaf may outlive them until the registry's next completed collection.
    }
    for (size_t pass = 0; pass < 2; ++pass)
    {
        size_t slot = window.freeTargets.size();
        {
            std::lock_guard lock(window.mutex);
            if (window.stopped || (window.policy && window.policy->Complete()) || window.allocating) break;
            for (size_t i = 0; i < window.freeTargets.size(); ++i)
                if (!window.freeTargets[i]) { slot = i; break; }
            if (slot == window.freeTargets.size()) break;
            window.allocating = true;
        }
        std::optional<TemporalTargets> allocated;
        try { allocated = AllocateTemporalTargets(window); }
        catch (...) {
            { std::lock_guard lock(window.mutex); window.allocating = false; }
            StopTemporalWindow(window, "bounded fresh target allocation refused");
            break;
        }
        {
            std::lock_guard lock(window.mutex);
            window.allocating = false;
            if (!window.stopped && !(window.policy && window.policy->Complete()) && !window.freeTargets[slot])
                window.freeTargets[slot] = std::move(allocated);
        }
    }
    std::array<std::optional<TemporalTargets>, 2> unused;
    bool finished = false;
    {
        std::lock_guard lock(window.mutex);
        finished = window.stopped || (window.policy && window.policy->Complete());
        if (finished)
            for (size_t i = 0; i < unused.size(); ++i) unused[i] = std::move(window.freeTargets[i]);
        bool undrained = false;
        if (window.policy)
            for (const auto& frame : window.frames)
                undrained |= frame && window.policy->ConsumerEmbedded(frame->temporalKey) &&
                             !window.policy->ConsumerReturned(frame->temporalKey);
        if (finished && !undrained)
        { lightingRequested.store(false); lightingAttempted.store(true); }
    }
    // Moved leaf owners are released outside the window lock; no COM backed
    // destructor participates in policy ordering or holds up original Execute.
    uint32_t preparedCount = 0, sceneCount = 0, returnedCount = 0, completeCount = 0;
    for (auto* frame : frames)
        if (frame)
        {
            std::lock_guard lock(frame->mutex);
            preparedCount += frame->denoise != nullptr || frame->retired;
            sceneCount += frame->sceneRecorded;
            returnedCount += frame->returned;
            completeCount += frame->retired;
        }
    std::shared_ptr<FSRD::PrivateDenoise::Session> completedSession;
    {
        std::lock_guard lock(window.mutex);
        if (completeCount == 32 && window.policy && window.policy->Complete() && !window.returnEvidencePending)
            completedSession = std::move(window.session);
    }
    // Last native consumer fence has completed for every Work. Provider context
    // destruction is outside controller locks; any remaining completed ticket
    // leaf independently owns its context/resources until registry collection.
    Json ledger;
    {
        std::lock_guard lock(window.mutex);
        const bool complete = window.policy && window.policy->Complete();
        const bool pending = window.returnEvidencePending || window.pendingWarmup.Valid() || std::any_of(window.pendingCalls.begin(), window.pendingCalls.end(),
                                                                       [](const auto& value) { return value.Valid(); });
        if ((!window.stopped && !complete) || pending || window.ledgerSaved) return;
        if (complete && returnedCount != 32) return; // Wait for the next nonblocking CPU poll, not a GPU wait.
        ledger = { { "schema", "optiscaler.fsr_rr.temporal_window_ledger.v1" }, { "epoch", window.epoch },
            { "complete", complete && !window.ledgerEvidenceLost }, { "stopped", bool(window.stopped) },
            { "failure", window.failure }, { "return_evidence_lost", window.ledgerEvidenceLost },
            { "returned_frames", window.policy ? window.policy->CommittedFrames() : 0 },
            { "prepared_frames", preparedCount }, { "scene_recorded_frames", sceneCount },
            { "native_returned_frames", returnedCount }, { "fence_complete_frames_at_ledger", completeCount },
            { "queue_identity", uintptr_t(window.queueIdentity.Get()) }, { "frames", window.ledger },
            { "clock", "selected_original_Fog_draw_CPU_interval_not_native_simulation_delta" },
            { "native_world_origin_epoch_proven", false }, { "GPU_complete", false } };
        window.ledgerSaved = true; // One bounded write attempt; a failure is not completion evidence.
    }
    const auto path = Util::ExePath().parent_path() / window.ledgerRelative;
    const auto encoded = ledger.dump(2);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("temporal ledger file creation refused");
    struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close { file };
    DWORD written = 0;
    if (encoded.size() > 1024 * 1024 || !WriteFile(file, encoded.data(), DWORD(encoded.size()), &written, nullptr) ||
        written != encoded.size() || !FlushFileBuffers(file))
        throw std::runtime_error("temporal ledger write incomplete");
    LOG_INFO("[FSRRR temporal] ledger saved {}; original late SR route remains fixed", path.string());
}

uint64_t AdmitTemporalSubmission(TemporalWindow& window, ID3D12CommandQueue* queue, UINT count,
                                ID3D12CommandList* const* lists)
{
    if (!count) return 0;
    if (!queue || !lists || count > ResetPolicy::Policy::MaxExecuteLists) PrivateResetFatal();
    ComPtr<IUnknown> queueId, deviceId;
    if (FAILED(queue->QueryInterface(IID_PPV_ARGS(&queueId))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&deviceId))))
        PrivateResetFatal();
    const ResetPolicy::Queue observedQueue { uintptr_t(queueId.Get()), uintptr_t(deviceId.Get()),
                                             queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT };
    std::array<ResetPolicy::Recording, ResetPolicy::Policy::MaxExecuteLists> recordings {};
    std::array<ComPtr<IUnknown>, ResetPolicy::Policy::MaxExecuteLists> identities;
    for (UINT i = 0; i < count; ++i)
    {
        if (!lists[i] || FAILED(lists[i]->QueryInterface(IID_PPV_ARGS(&identities[i])))) PrivateResetFatal();
        recordings[i].list = uintptr_t(identities[i].Get());
    }
    {
        auto& data = Data(); std::lock_guard lock(data.mutex);
        for (UINT i = 0; i < count; ++i)
        {
            const auto found = data.lists.find(identities[i].Get());
            if (captureTrackingValid.load() && found != data.lists.end() && found->second.known)
                recordings[i].generation = found->second.generation;
        }
    }
    std::lock_guard lock(window.mutex);
    if (!window.warmupReturned)
    {
        unsigned matched = 0;
        for (UINT i = 0; i < count; ++i)
            if (window.warmupRecording.list && recordings[i] == window.warmupRecording) ++matched;
        if (!matched) return 0;
        if (matched != 1 || window.pendingWarmup.Valid() || !observedQueue.direct ||
            deviceId.Get() != window.deviceIdentity.Get()) PrivateResetFatal();
        window.queueIdentity = queueId;
        window.pendingWarmup = { window.epoch, ++window.warmupSerial, uintptr_t(queueId.Get()) };
        return TemporalToken | WarmupToken | window.pendingWarmup.serial;
    }
    if (!window.policy) return 0;
    const auto decision = window.policy->BeforeExecute(observedQueue,
                                                       std::span<const ResetPolicy::Recording>(recordings.data(), count));
    if (!decision.allowed) PrivateResetFatal();
    if (!decision.receipt.Valid()) return 0;
    auto empty = std::find_if(window.pendingCalls.begin(), window.pendingCalls.end(), [](const auto& entry) { return !entry.Valid(); });
    if (empty == window.pendingCalls.end() || decision.receipt.serial >= WarmupToken) PrivateResetFatal();
    *empty = decision.receipt;
    return TemporalToken | decision.receipt.serial;
}

void ReturnedTemporalSubmission(TemporalWindow& window, uint64_t token)
{
    if (token & WarmupToken)
    {
        std::lock_guard lock(window.mutex);
        if (!window.pendingWarmup.Valid() || window.pendingWarmup.serial != (token & ~(TemporalToken | WarmupToken)))
            PrivateResetFatal();
        window.warmupReturned = true; window.pendingWarmup = {};
        return;
    }
    WindowPolicy::ReturnDecision returned;
    PrivateResetPacket* frame = nullptr;
    {
        std::lock_guard lock(window.mutex);
        const auto found = std::find_if(window.pendingCalls.begin(), window.pendingCalls.end(),
            [&](const auto& entry) { return entry.Valid() && entry.serial == (token & ~TemporalToken); });
        if (found == window.pendingCalls.end() || !window.policy) PrivateResetFatal();
        returned = window.policy->AfterExecute(*found); *found = {};
        if (!returned.allowed) PrivateResetFatal();
        if (!returned.consumer.Valid()) return; // Producer-only return never advances AMD or camera history.
        ++window.returnEvidencePending;
        frame = window.frames[returned.consumer.index].get();
    }
    if (!frame) PrivateResetFatal();
    std::shared_ptr<FSRD::PrivateDenoise::Work> work;
    std::optional<ResetSource::TemporalSource> current;
    {
        std::lock_guard lock(frame->mutex);
        if (frame->returned || !frame->consumerSealed || !frame->sceneRecorded || !frame->denoise || !frame->timedSource)
            PrivateResetFatal();
        work = frame->denoise; current = frame->timedSource;
    }
    {
        // Session acknowledgement performs only its own CPU state transition.
        // No provider/engine/queue call occurs here. Camera and admission commit
        // are atomic to a future Fog preparation under this same controller lock.
        std::lock_guard lock(window.mutex);
        if (!window.policy->ConsumerReturned(returned.consumer) || !work->Recorded() ||
            !window.session->AcknowledgeExecuted(*work, uintptr_t(window.queueIdentity.Get()))) PrivateResetFatal();
        window.previous = TemporalCamera::PreviousFrame { current->current.rawSnapshot, current->current.motionScale,
                                                          frame->delta, current->current.frame, window.epoch };
        if (!window.stopped && !window.policy->CommitConsumer(returned.consumer)) PrivateResetFatal();
    }
    { std::lock_guard lock(frame->mutex); frame->returned = true; }
    // Ledger/logging allocation cannot unwind or retroactively change the
    // successful native return and CPU acknowledgement above.
    try {
        const auto& effective = work->EffectiveParameters();
        std::array<uint32_t, 16> previousView {};
        std::array<uint32_t, 4> previousDepth {};
        std::memcpy(previousView.data(), &effective.conversion.PrevViewMatrix, sizeof(previousView));
        std::memcpy(previousDepth.data(), &effective.conversion.PreviousDepthProjection, sizeof(previousDepth));
        Json entry = { { "ordinal", returned.consumer.index + 1 }, { "frame", returned.consumer.frame },
            { "epoch", window.epoch }, { "consumer_returned", true }, { "session_acknowledged", true },
            { "view", current->current.view }, { "frame_source_object", current->current.object },
            { "render_extent", { current->current.width, current->current.height } },
            { "consumer_list_identity", frame->consumer.list }, { "consumer_recording_generation", frame->consumer.generation },
            { "direct_queue_identity", uintptr_t(window.queueIdentity.Get()) },
            { "work_identity", uintptr_t(work.get()) }, { "session_identity", uintptr_t(window.session.get()) },
            { "scene_recorded", true }, { "delta_ms", frame->delta }, { "fog_cpu_timestamp_ms", frame->fogTimestamp },
            { "previous_fog_cpu_timestamp_ms", frame->previousFogTimestamp }, { "native_reset", current->nativeResetRequested },
            { "camera", current->current.camera }, { "first_RESET_only", returned.consumer.index == 0 },
            { "conversion_flags", effective.conversion.Flags }, { "dispatch_flags", effective.dispatch.flags },
            { "previous_view_words", previousView }, { "previous_depth_projection_words", previousDepth },
            { "camera_delta_words", { std::bit_cast<uint32_t>(effective.dispatch.cameraPositionDelta.x),
                std::bit_cast<uint32_t>(effective.dispatch.cameraPositionDelta.y),
                std::bit_cast<uint32_t>(effective.dispatch.cameraPositionDelta.z) } } };
        { std::lock_guard lock(window.mutex); window.ledger[returned.consumer.index] = std::move(entry); }
        LOG_INFO("[FSRRR temporal] consumer returned and history committed ordinal={} frame={} epoch={}",
                 returned.consumer.index + 1, returned.consumer.frame, window.epoch);
    } catch (...) { std::lock_guard lock(window.mutex); window.ledgerEvidenceLost = true; }
    { std::lock_guard lock(window.mutex); --window.returnEvidencePending; }
}
} // namespace

void PollTemporalWindow(ID3D12Device* device, UINT width, UINT height) noexcept
{
    if (auto* window = temporalWindow.load(std::memory_order_acquire))
    {
        Metadata([&] {
            try { MaintainTemporalWindow(*window); }
            catch (const std::exception& error) { StopTemporalWindow(*window, error.what()); }
        });
        return;
    }
    if (!active.load() || !captureEnabled.load() || !captureTrackingValid.load() || !FSRD::PreFogSession::LateSrOnly() ||
        privateResetPacket.load() || rgbIdentityPacket.load()) return;
    static std::atomic<ULONGLONG> nextPoll { 0 };
    auto due = nextPoll.load(); const auto now = GetTickCount64();
    if (now < due || !nextPoll.compare_exchange_strong(due, now + 1000)) return;
    Metadata([&] {
        std::unique_lock requestLock(captureRequestMutex, std::try_to_lock);
        if (!requestLock) return;
        const auto path = Util::ExePath().parent_path() / L"FSRRR-prefog-temporal.request";
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        if (privateResetArming.exchange(true, std::memory_order_acq_rel)) return;
        struct Finish { ~Finish() { privateResetArming.store(false, std::memory_order_release); } } finish;
        try {
            const auto fog = FSRDFogLayerCapture::GetStatus(), guides = FSRDFogLayerCapture::GetEarlyGuideStatus();
            if (!device || !width || !height || width > 8192 || height > 8192 || TemporalRecordingRequested() ||
                privateResetPacket.load() || rgbIdentityPacket.load() || captureStarted.load() || lightingRequested.load() ||
                lightingAttempted.load() || earlyRequested.load() || earlyAttempted.load() || rayCopyAttempted.load() ||
                fog.queued || fog.busy || fog.attempted || guides.queued || guides.busy || guides.attempted ||
                !rayBindingsAuthenticated.load() || !fogDepthAuthenticated.load() ||
                authenticatedImage.load() != uintptr_t(GetModuleHandleW(nullptr)) ||
                !MatchLiveCode(authenticatedImage.load(), FogTopologyCode) || std::filesystem::file_size(path) > 4096)
                throw std::runtime_error("temporal window requires an unused authenticated fixed-SR session");
            Json controls;
            { std::ifstream file(path, std::ios::binary); file >> controls; } // Closed before DeleteFileW.
            if (controls.at("mode") != "temporal_window_32" || controls.at("frame_count") != 32 ||
                controls.at("delta_source") != "selected_Fog_draw_CPU_interval_not_native_delta" ||
                controls.at("continuity") != "experimental_software_epoch_stable_view_no_native_origin_proof")
                throw std::runtime_error("temporal window requires explicit CPU-clock/software-continuity experiment controls");
            auto window = std::make_unique<TemporalWindow>();
            window->device = device; window->width = width; window->height = height;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&window->deviceIdentity))))
                throw std::runtime_error("temporal device identity unavailable");
            window->provider = controls.at("provider_id").get<uint64_t>();
            const auto& settings = controls.at("settings");
            if (!window->provider || !settings.is_object() || settings.size() != 6)
                throw std::runtime_error("temporal explicit provider/settings incomplete");
            window->settings = { settings.at("1").get<float>(), settings.at("2").get<float>(), settings.at("3").get<float>(),
                settings.at("4").get<float>(), settings.at("5").get<float>(), settings.at("6").get<float>() };
            window->epoch = ++nextTemporalEpoch;
            if (!window->epoch) throw std::runtime_error("temporal epoch exhausted");
            window->ledgerRelative = std::format("FSRRR-temporal-{}-{}-{}.json", GetCurrentProcessId(), now, window->epoch);
            const char* error = nullptr;
            window->session = FSRD::PrivateDenoise::CreateSession(device,
                { { width, height }, window->provider, window->settings, window->epoch, 32 }, &error);
            if (!window->session) throw std::runtime_error(error && *error ? error : "temporal Session preparation refused");
            for (auto& free : window->freeTargets) free = AllocateTemporalTargets(*window);
            if (!DeleteFileW(path.c_str())) throw std::runtime_error("temporal marker consumption refused");
            temporalWindow.store(window.release(), std::memory_order_release);
            lightingRequestedAt.store(GetTickCount64()); lightingRequested.store(true);
            LOG_INFO("[FSRRR temporal] armed warm-up {}x{}; 32-frame bound, one Session, first RESET only, final-only capture; no native timing/origin proof", width, height);
        } catch (const std::exception& error) { LOG_WARN("[FSRRR temporal] arm refused: {}", error.what()); }
    });
}

uint64_t AdmitPrivateResetSubmission(ID3D12CommandQueue* queue, UINT count,
                                      ID3D12CommandList* const* lists) noexcept
{
    if (auto* window = temporalWindow.load(std::memory_order_acquire))
    {
        try { return AdmitTemporalSubmission(*window, queue, count, lists); }
        catch (...) { PrivateResetFatal(); }
    }
    auto* packet = privateResetPacket.load(std::memory_order_acquire);
    if (!packet) return 0;
    try
    {
        if (!count) return 0;
        if (!queue || !lists || count > ResetPolicy::Policy::MaxExecuteLists)
            PrivateResetFatal();
        ComPtr<IUnknown> queueIdentity, deviceIdentity;
        if (FAILED(queue->QueryInterface(IID_PPV_ARGS(&queueIdentity))) ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&deviceIdentity))))
            PrivateResetFatal();
        std::array<ResetPolicy::Recording, ResetPolicy::Policy::MaxExecuteLists> recordings {};
        std::array<ComPtr<IUnknown>, ResetPolicy::Policy::MaxExecuteLists> identities;
        for (UINT i = 0; i < count; ++i)
        {
            if (!lists[i] || FAILED(lists[i]->QueryInterface(IID_PPV_ARGS(&identities[i]))))
                PrivateResetFatal();
            recordings[i].list = uintptr_t(identities[i].Get());
        }
        {
            auto& data = Data();
            std::lock_guard lock(data.mutex);
            for (UINT i = 0; i < count; ++i)
            {
                const auto found = data.lists.find(identities[i].Get());
                if (captureTrackingValid.load() && found != data.lists.end() && found->second.known)
                    recordings[i].generation = found->second.generation;
            }
        }
        ResetPolicy::Decision decision;
        {
            std::lock_guard lock(packet->mutex);
            decision = packet->policy.BeforeExecute(
                { uintptr_t(queueIdentity.Get()), uintptr_t(deviceIdentity.Get()),
                  queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT },
                std::span<const ResetPolicy::Recording>(recordings.data(), count));
            if (decision.token) packet->queueIdentity = queueIdentity;
        }
        if (!decision.Allowed())
        {
            try { LOG_ERROR("[FSRRR private RESET] submission refused failure={} count={}", unsigned(decision.failure), count); }
            catch (...) {}
            PrivateResetFatal();
        }
        if (decision.token)
        {
            // Logging is NOT the admission mechanism and cannot unwind it.
            try { LOG_INFO("[FSRRR private RESET] pre-submit admitted token={} route={} queue={:x} count={}",
                decision.token, unsigned(decision.admission), uintptr_t(queueIdentity.Get()), count); }
            catch (...) {}
        }
        return decision.token;
    }
    catch (...) { PrivateResetFatal(); } // Never best-effort Metadata around this gate.
}

void ReturnedPrivateResetSubmission(uint64_t token) noexcept
{
    if (!token) return;
    if (token & TemporalToken)
    {
        auto* window = temporalWindow.load(std::memory_order_acquire);
        if (!window) PrivateResetFatal();
        try { ReturnedTemporalSubmission(*window, token); }
        catch (...) { PrivateResetFatal(); }
        return;
    }
    auto* packet = privateResetPacket.load(std::memory_order_acquire);
    if (!packet) PrivateResetFatal();
    {
        std::lock_guard lock(packet->mutex);
        if (!packet->policy.AfterExecute(token)) PrivateResetFatal();
    }
    try { LOG_INFO("[FSRRR private RESET] native Execute returned token={}", token); }
    catch (...) {}
}

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
            // These bodies include the Fog node and texture binder: authenticate
            // all of them before this initialization transaction patches either.
            if (captures && std::all_of(std::begin(FSRD::CyberpunkFogDepth::Code), std::end(FSRD::CyberpunkFogDepth::Code),
                [&](const auto& code) { return MatchLiveCode(image, code); }))
                fogDepthAuthenticated.store(true);
            if (captures && std::all_of(std::begin(ProducerCode), std::end(ProducerCode),
                                       [&](const auto& code) { return MatchLiveCode(image, code); }))
                originalGBufferInitializer = reinterpret_cast<GBufferInitializer>(image + GBufferInitializerRva);
            if (captures && std::all_of(std::begin(LightingCode), std::end(LightingCode),
                                       [&](const auto& code) { return MatchLiveCode(image, code); }))
            {
                originalLightingNode = reinterpret_cast<FogNode>(image + LightingNodeRva);
                originalFullscreenHelper = reinterpret_cast<FullscreenHelper>(image + FullscreenHelperRva);
                originalBindTextures = reinterpret_cast<BindTextures>(image + FSRD::CyberpunkLightingSource::BinderRva);
                if (std::all_of(std::begin(FSRD::CyberpunkLightingConstants::Code), std::end(FSRD::CyberpunkLightingConstants::Code),
                    [&](const auto& code) { return MatchLiveCode(image, { code.rva, code.bytes, code.sha256 }); }))
                    originalUploadLightingConstants = reinterpret_cast<UploadLightingConstants>(image + FSRD::CyberpunkLightingConstants::UploadRva);
            }
            if (captures && std::all_of(std::begin(FSRD::CyberpunkRayConstants::Code), std::end(FSRD::CyberpunkRayConstants::Code),
                [&](const auto& code) { return MatchLiveCode(image, { code.rva, code.bytes, code.sha256 }); }))
            {
                originalRayNode = reinterpret_cast<FogNode>(image + FSRD::CyberpunkRayConstants::NodeRva);
                originalUploadRayConstants = reinterpret_cast<UploadLightingConstants>(image + FSRD::CyberpunkRayConstants::UploadRva);
                // The ordinary texture binder below will be detoured too. All
                // these body hashes must be checked BEFORE either patch exists.
                if (originalBindTextures && std::all_of(std::begin(FSRD::CyberpunkRayBindings::Code),
                    std::end(FSRD::CyberpunkRayBindings::Code),
                    [&](const auto& code) { return MatchLiveCode(image, { code.rva, code.bytes, code.sha256 }); }))
                {
                    originalBindUavs = reinterpret_cast<BindUavs>(image + 0x153f94);
                    rayBindingsAuthenticated.store(true);
                    if (MatchLiveCode(image, FSRD::CyberpunkRayAccess::CleanupCode))
                        originalRayCleanup = reinterpret_cast<RayCleanup>(image + FSRD::CyberpunkRayAccess::CleanupCode.rva);
                }
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
                if (error == NO_ERROR && originalBindTextures)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalBindTextures), HookBindTextures);
                if (error == NO_ERROR && originalBindUavs)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalBindUavs), HookBindUavs);
                if (error == NO_ERROR && originalUploadLightingConstants)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalUploadLightingConstants), HookUploadLightingConstants);
                if (error == NO_ERROR && originalRayNode)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalRayNode), HookRayNode);
                if (error == NO_ERROR && originalUploadRayConstants)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalUploadRayConstants), HookUploadRayConstants);
                if (error == NO_ERROR && originalRayCleanup)
                    error = DetourAttach(reinterpret_cast<PVOID*>(&originalRayCleanup), HookRayCleanup);
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
                originalBindTextures = nullptr;
                originalBindUavs = nullptr;
                rayBindingsAuthenticated.store(false);
                fogDepthAuthenticated.store(false);
                originalUploadLightingConstants = nullptr;
                originalRayNode = nullptr;
                originalUploadRayConstants = nullptr;
                originalRayCleanup = nullptr;
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

// Called only with hookMutex held. The first base list may not expose List4:
// retry optional discovery on later lists even after Draw's hook is installed.
// Query the actual interface; never assume the base and List4 pointers coincide.
void HookOptionalList4Metadata(ID3D12GraphicsCommandList* list)
{
    if (!captureEnabled.load()) return;
    const bool needBegin = !originalBeginRenderPass, needEnd = !originalEndRenderPass;
    const bool needRays = rayBindingsAuthenticated.load() && !originalDispatchRays;
    if (!needBegin && !needEnd && !needRays) return;
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list4)))) return;
    auto** table4 = *reinterpret_cast<void***>(list4.Get());
    if (needBegin) originalBeginRenderPass = reinterpret_cast<BeginRenderPass>(table4[68]);
    if (needEnd) originalEndRenderPass = reinterpret_cast<EndRenderPass>(table4[69]);
    // ID3D12GraphicsCommandList4's SDK member ABI; authenticated native call uses
    // vtable+0x260. The GPU-address-bearing description is read as 104 raw bytes.
    if (needRays) originalDispatchRays = reinterpret_cast<DispatchRays>(table4[76]);
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR && needBegin)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalBeginRenderPass), HookBeginRenderPass);
        if (error == NO_ERROR && needEnd)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalEndRenderPass), HookEndRenderPass);
        if (error == NO_ERROR && needRays)
            error = DetourAttach(reinterpret_cast<PVOID*>(&originalDispatchRays), HookDispatchRays);
        if (error == NO_ERROR) error = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (error != NO_ERROR)
    {
        if (needBegin) originalBeginRenderPass = nullptr;
        if (needEnd) originalEndRenderPass = nullptr;
        if (needRays) { originalDispatchRays = nullptr; rayBindingsAuthenticated.store(false); }
        if (needBegin || needEnd) captureTrackingValid.store(false);
    }
    LOG_INFO("[FSRRR ray bindings] optional List4 metadata hooks result={} DispatchRays={}", error,
             originalDispatchRays != nullptr);
}

void HookCommandList(ID3D12GraphicsCommandList* list)
{
    if (!active.load() || !list)
        return;
    std::lock_guard lock(hookMutex);
    if (originalDraw)
    {
        HookOptionalList4Metadata(list);
        return;
    }
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
    if (error == NO_ERROR) HookOptionalList4Metadata(list);
}
} // namespace FSRDCyberpunkFogProbe

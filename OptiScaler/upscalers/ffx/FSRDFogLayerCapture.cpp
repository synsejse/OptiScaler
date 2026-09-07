#include "pch.h"
#include "FSRDFogLayerCapture.h"
#include "resource_tracking/FSRDSubmission.h"
#include "Util.h"
#include <json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace FSRDFogLayerCapture
{
namespace
{
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
constexpr UINT64 MaxBytes = 256ull * 1024 * 1024;
constexpr UINT MaxDimension = 8192;
constexpr ULONGLONG CompletionTimeoutMs = 30000;
constexpr size_t MaxProvenanceBytes = 256 * 1024;

struct Entry
{
    const char* role = nullptr;
    std::string filename;
    Texture source;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 bytes = 0;
    const char* componentType = nullptr;
    UINT pixelBytes = 0;
    const char* channels = "RGBA";
};

struct Batch
{
    bool guidesOnly = false;
    std::array<Entry, 3> entries;
    std::optional<Entry> boundCb12;
    std::optional<std::array<Entry, 3>> earlyGuides;
    std::optional<Entry> exposureWords; // Standalone guide batches only; never a floating fog layer.
    std::optional<Entry> lightingT8; // Owned copy-readable encoded input; never a reconstructed scene layer.
    std::optional<std::array<Entry, 2>> rayCopies; // Private original-ray snapshots; units remain caller evidence.
    std::optional<Entry> hardwareDepth; // Fog-only private scalar snapshot, never a base scene layer.
    std::optional<std::array<Entry, 3>> privateReset; // Fog-only independent RESET diagnostics, no scene substitution.
    std::optional<Entry> rgbIdentity; // Private post-identity/pre-Fog snapshot; success is not inferred.
    std::shared_ptr<void> keepAlive;
    Json metadata;
    std::filesystem::path directory;
};

enum class RequestKind { FogLayers, EarlyGuides };

struct Registry
{
    std::mutex mutex;
    std::atomic<bool> requested { false };
    Status status { .message = "No native capture requested." };
    // Ticket owns Batch through FSRDSubmission. Batch must NOT own Ticket.
    // These additional references survive worker errors/timeouts without a GPU wait.
    std::shared_ptr<Batch> pending;
    std::shared_ptr<FSRDSubmission::Ticket> ticket;
};

Registry& GetRegistry(RequestKind kind)
{
    // Two fixed slots: one recorded attempt of each kind, including failures.
    // Each retains the existing 256 MiB readback cap (512 MiB combined), with no
    // time/latest lookup or cross-kind pending/ticket replacement. Abandoned GPU
    // work survives DLL teardown instead of releasing still-referenced resources.
    static auto* registries = new std::array<Registry, 2>;
    return (*registries)[kind == RequestKind::FogLayers ? 0 : 1];
}

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::format("{} ({:08X})", operation, static_cast<UINT>(result)));
}

std::string Timestamp(const char* prefix = "fog")
{
    SYSTEMTIME now {};
    GetSystemTime(&now);
    return std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}Z-{}", prefix, now.wYear, now.wMonth,
                       now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
}

void FinishStatus(RequestKind kind, bool complete, const std::string& message, bool releaseStorage)
{
    // COM-backed storage must be destroyed only after unlocking (Release can enter hooks).
    std::shared_ptr<Batch> retiredBatch;
    std::shared_ptr<FSRDSubmission::Ticket> retiredTicket;
    auto& registry = GetRegistry(kind);
    {
        std::lock_guard lock(registry.mutex);
        registry.status.busy = false;
        registry.status.complete = complete;
        registry.status.message = message;
        if (releaseStorage)
        {
            retiredBatch = std::move(registry.pending);
            retiredTicket = std::move(registry.ticket);
        }
    }
}

void CheckSameDevice(ID3D12Device* device, ID3D12DeviceChild* child)
{
    ComPtr<IUnknown> expected, actual;
    Check(device->QueryInterface(IID_PPV_ARGS(&expected)), "fog capture device identity unavailable");
    Check(child->GetDevice(IID_PPV_ARGS(&actual)), "fog capture resource device unavailable");
    if (expected.Get() != actual.Get())
        throw std::runtime_error("fog capture resources/list must belong to the supplied device");
}

bool IsExposureWordsTexture(const D3D12_RESOURCE_DESC& desc, DXGI_FORMAT viewFormat,
                            UINT subresource, D3D12_RESOURCE_STATES state) noexcept
{
    constexpr D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return viewFormat == DXGI_FORMAT_R32G32B32A32_UINT && desc.Format == viewFormat &&
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == 2 && desc.Height == 1 &&
        desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
        desc.SampleDesc.Quality == 0 && subresource == 0 && state == RequiredState;
}

bool IsLightingT8Texture(const D3D12_RESOURCE_DESC& desc, DXGI_FORMAT viewFormat,
                         UINT subresource, D3D12_RESOURCE_STATES state, UINT width, UINT height) noexcept
{
    constexpr D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE;
    // The passed tag is the required read mask, not a claim about the native
    // engine-managed mask. COPY_SOURCE suppresses both barriers in RecordCopy.
    return width > 0 && height > 0 && width <= MaxDimension && height <= MaxDimension &&
        viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format == viewFormat &&
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width && desc.Height == height &&
        desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
        desc.SampleDesc.Quality == 0 && subresource == 0 && state == RequiredState;
}

bool IsRayCopyTexture(const D3D12_RESOURCE_DESC& desc, DXGI_FORMAT viewFormat,
                      UINT subresource, D3D12_RESOURCE_STATES state, UINT width, UINT height,
                      bool rayHit) noexcept
{
    constexpr D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto format = rayHit ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    return width > 0 && height > 0 && width <= MaxDimension && height <= MaxDimension &&
        viewFormat == format && desc.Format == format &&
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width && desc.Height == height &&
        desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
        desc.SampleDesc.Quality == 0 && subresource == 0 && state == RequiredState;
}

void PrepareEntry(ID3D12Device* device, Entry& entry, bool boundCb12 = false, bool guideAlbedo = false,
                  bool exposureWords = false, bool rayHit = false)
{
    if (!entry.source.resource)
        throw std::runtime_error("fog capture requires all three immutable textures");
    CheckSameDevice(device, entry.source.resource.Get());
    const auto desc = entry.source.resource->GetDesc();
    if (rayHit)
    {
        // R32_FLOAT is admitted ONLY for explicit private scalar companion roles
        // (ray hit or Fog hardware depth, selected by the caller below);
        // none of the original fog layers or guide formats gain scalar support.
        if (boundCb12 || guideAlbedo || exposureWords || desc.Width > MaxDimension ||
            !IsRayCopyTexture(desc, entry.source.viewFormat, entry.source.subresource, entry.source.state,
                              static_cast<UINT>(desc.Width), desc.Height, true))
            throw std::runtime_error("scalar companion requires exact private mip0 R32_FLOAT in state0xc0");
        entry.componentType = "float32";
        entry.pixelBytes = 4;
        entry.channels = "R";
        entry.filename = std::string(entry.role) + ".r32f";
    }
    else if (exposureWords)
    {
        if (boundCb12 || guideAlbedo ||
            !IsExposureWordsTexture(desc, entry.source.viewFormat, entry.source.subresource, entry.source.state))
            throw std::runtime_error("exposure words require exact private 2x1 mip0 RGBA32_UINT in state0xc0");
        entry.componentType = "uint32";
        entry.pixelBytes = 16;
        entry.filename = std::string(entry.role) + ".rgba32u";
    }
    else if (boundCb12)
    {
        // A companion must not broaden the accepted formats/extents of any of
        // the original three floating-point layers.
        if (entry.source.viewFormat != DXGI_FORMAT_R32G32B32A32_UINT ||
            desc.Format != DXGI_FORMAT_R32G32B32A32_UINT || desc.Width != 5 || desc.Height != 1 ||
            desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Quality != 0 ||
            entry.source.subresource != 0)
            throw std::runtime_error("bound cb12 companion requires exact private 5x1 mip0 RGBA32_UINT");
        entry.componentType = "uint32";
        entry.pixelBytes = 16;
        entry.filename = std::string(entry.role) + ".rgba32u";
    }
    else if (guideAlbedo)
    {
        if (entry.source.viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM ||
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            throw std::runtime_error("guide albedo companion requires exact typed RGBA8_UNORM");
        entry.componentType = "unorm8";
        entry.pixelBytes = 4;
        entry.filename = std::string(entry.role) + ".rgba8unorm";
    }
    else if (entry.source.viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS))
    {
        entry.componentType = "float16";
        entry.pixelBytes = 8;
        entry.filename = std::string(entry.role) + ".rgba16f";
    }
    else if (entry.source.viewFormat == DXGI_FORMAT_R32G32B32A32_FLOAT &&
             (desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || desc.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS))
    {
        entry.componentType = "float32";
        entry.pixelBytes = 16;
        entry.filename = std::string(entry.role) + ".rgba32f";
    }
    else
        throw std::runtime_error("fog capture only supports native RGBA16F/RGBA32F; refusing format conversion");
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.MipLevels == 0 || desc.DepthOrArraySize == 0 ||
        entry.source.subresource >= UINT(desc.MipLevels) * desc.DepthOrArraySize)
        throw std::runtime_error("fog capture requires valid single-sample Texture2D subresources");

    device->GetCopyableFootprints(&desc, entry.source.subresource, 1, 0, &entry.footprint, &entry.rows,
                                  &entry.rowBytes, &entry.bytes);
    const auto& footprint = entry.footprint.Footprint;
    if (footprint.Width == 0 || footprint.Height == 0 || footprint.Width > MaxDimension ||
        footprint.Height > MaxDimension || footprint.Depth != 1 || entry.rows != footprint.Height ||
        entry.rowBytes != UINT64(footprint.Width) * entry.pixelBytes || footprint.RowPitch < entry.rowBytes ||
        entry.footprint.Offset + UINT64(entry.rows - 1) * footprint.RowPitch + entry.rowBytes > entry.bytes ||
        entry.bytes == 0 || entry.bytes > MaxBytes)
        throw std::runtime_error("fog capture footprint exceeds limits or does not match the native RGBA format");
}

void AllocateReadback(ID3D12Device* device, Entry& entry)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = entry.bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&entry.readback)),
          "fog capture readback allocation failed");
}

Json Describe(const Entry& entry)
{
    const auto desc = entry.source.resource->GetDesc();
    return { { "role", entry.role },
             { "file", entry.filename },
             { "width", entry.footprint.Footprint.Width },
             { "height", entry.footprint.Footprint.Height },
             { "channels", entry.channels },
             { "component_type", entry.componentType },
             { "pixel_bytes", entry.pixelBytes },
             { "byte_order", "little_endian" },
             { "row_order", "native_texture_no_flip" },
             { "row_bytes", entry.rowBytes },
             { "file_bytes", entry.rowBytes * entry.rows },
             { "gpu_row_pitch", entry.footprint.Footprint.RowPitch },
             { "readback_bytes", entry.bytes },
             { "subresource", entry.source.subresource },
             { "resource_format", static_cast<UINT>(desc.Format) },
             { "view_format", static_cast<UINT>(entry.source.viewFormat) },
             { "state_restored", static_cast<UINT>(entry.source.state) },
             { "snapshot_identity", std::format("0x{:X}", reinterpret_cast<uintptr_t>(entry.source.resource.Get())) } };
}

void RecordCopy(ID3D12GraphicsCommandList* list, const Entry& entry)
{
    const bool transition = (entry.source.state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = entry.source.resource.Get();
    barrier.Transition.Subresource = entry.source.subresource;
    barrier.Transition.StateBefore = entry.source.state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (transition)
        list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source {}, target {};
    source.pResource = entry.source.resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = entry.source.subresource;
    target.pResource = entry.readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = entry.footprint;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);

    if (transition)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }
}

void WriteManifest(const Batch& batch)
{
    std::ofstream file(batch.directory / "manifest.json", std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file << batch.metadata.dump(2) << '\n';
    file.close();
}

bool HasZeroExposurePadding(const void* pixels, UINT64 rowBytes, UINT rows) noexcept
{
    if (!pixels || rowBytes != 32 || rows != 1) return false;
    uint32_t padding = 0;
    std::memcpy(&padding, static_cast<const char*>(pixels) + 28, sizeof(padding));
    return padding == 0;
}

void WriteEntry(const Batch& batch, const Entry& entry, bool exposureWords = false)
{
    struct Mapping
    {
        ID3D12Resource* resource;
        void* data = nullptr;
        ~Mapping()
        {
            if (data)
            {
                const D3D12_RANGE noWrites { 0, 0 };
                resource->Unmap(0, &noWrites);
            }
        }
    } mapping { entry.readback.Get() };
    const D3D12_RANGE range { 0, static_cast<SIZE_T>(entry.bytes) };
    Check(entry.readback->Map(0, &range, &mapping.data), "fog capture readback map failed");
    if (!mapping.data)
        throw std::runtime_error("fog capture readback map returned null");
    const auto* pixels = static_cast<const char*>(mapping.data) + entry.footprint.Offset;
    if (exposureWords && !HasZeroExposurePadding(pixels, entry.rowBytes, entry.rows))
        throw std::runtime_error("exposure-word companion has invalid zero padding; native words not modified");

    const auto path = batch.directory / entry.filename;
    auto temporary = path;
    temporary += ".part";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    for (UINT y = 0; y < entry.rows; ++y)
    {
        const auto* row = pixels + UINT64(y) * entry.footprint.Footprint.RowPitch;
        file.write(row, static_cast<std::streamsize>(entry.rowBytes));
    }
    file.close();
    std::filesystem::rename(temporary, path);
}

bool SubmissionFailed(const std::shared_ptr<FSRDSubmission::Ticket>& ticket)
{
    auto& registry = FSRDSubmission::GetRegistry();
    std::lock_guard lock(registry.mutex);
    return ticket->signalFailed || (ticket->fence && ticket->fence->GetCompletedValue() == UINT64_MAX);
}

struct WorkerArgs
{
    HMODULE module = nullptr;
    std::shared_ptr<Batch> batch;
    std::shared_ptr<FSRDSubmission::Ticket> ticket;
};

void WriteWhenComplete(const WorkerArgs& args)
{
    // The batch's immutable origin selects its own admission slot, even when the
    // other kind records or completes concurrently. Fence semantics are unchanged.
    const auto kind = args.batch->guidesOnly ? RequestKind::EarlyGuides : RequestKind::FogLayers;
    const auto start = GetTickCount64();
    while (!FSRDSubmission::Complete(args.ticket))
    {
        if (SubmissionFailed(args.ticket))
        {
            FinishStatus(kind, false, "Native capture submission failed/device removed; GPU storage retained.", false);
            return;
        }
        if (GetTickCount64() - start >= CompletionTimeoutMs)
        {
            FinishStatus(kind, false, "Native capture completion timed out; GPU storage retained (no render-thread wait).", false);
            return;
        }
        Sleep(20); // Only the one-shot background writer waits, never the game's recording thread.
    }

    auto& batch = *args.batch;
    bool directoryCreated = false;
    try
    {
        std::filesystem::create_directories(batch.directory.parent_path());
        // Never overwrite an earlier capture, including timestamp/PID collisions.
        if (!std::filesystem::create_directory(batch.directory))
            throw std::runtime_error("fog capture directory already exists");
        directoryCreated = true;
        WriteManifest(batch); // Explicitly incomplete until all native files are closed successfully.
        for (const auto& entry : batch.entries)
            WriteEntry(batch, entry);
        if (batch.boundCb12)
            WriteEntry(batch, *batch.boundCb12);
        if (batch.earlyGuides)
            for (const auto& entry : *batch.earlyGuides)
                WriteEntry(batch, entry);
        if (batch.exposureWords)
            WriteEntry(batch, *batch.exposureWords, true);
        if (batch.lightingT8)
            WriteEntry(batch, *batch.lightingT8);
        if (batch.rayCopies)
            for (const auto& entry : *batch.rayCopies)
                WriteEntry(batch, entry);
        if (batch.hardwareDepth)
            WriteEntry(batch, *batch.hardwareDepth);
        if (batch.privateReset)
            for (const auto& entry : *batch.privateReset)
                WriteEntry(batch, entry);
        if (batch.rgbIdentity)
            WriteEntry(batch, *batch.rgbIdentity);
        batch.metadata["complete"] = true;
        WriteManifest(batch);
        FinishStatus(kind, true, batch.guidesOnly ? (batch.rayCopies ?
                         "Early guide capture saved: native guides, private ray snapshots and optional lighting companions." :
                         batch.lightingT8 ?
                         "Early guide capture saved: native guides, encoded lighting t8 and optional exposure words." :
                         batch.exposureWords ?
                         "Early guide capture saved: three native guides, raw exposure words and provenance." :
                         "Early guide capture saved: three native guide companions and provenance.") :
                         "Fog capture saved: three native floating-point RGBA layers and provenance.", true);
    }
    catch (const std::exception& error)
    {
        if (directoryCreated)
        {
            try
            {
                batch.metadata["complete"] = false;
                batch.metadata["error"] = error.what();
                WriteManifest(batch);
            }
            catch (...)
            {
                // Keep partial files for diagnosis; do not turn a disk error into a render failure.
            }
        }
        FinishStatus(kind, false, std::format("Native capture write failed: {}", error.what()), true);
    }
}

DWORD WINAPI WriterThread(void* raw)
{
    HMODULE module = nullptr;
    {
        // Finish every C++ destructor before releasing the module reference/ending this thread.
        std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(raw));
        module = args->module;
        try
        {
            WriteWhenComplete(*args);
        }
        catch (...)
        {
            // A failed worker never releases storage whose completion was not established.
            try
            {
                const auto kind = args->batch->guidesOnly ? RequestKind::EarlyGuides : RequestKind::FogLayers;
                FinishStatus(kind, false, "Native capture worker failed; GPU storage retained.", false);
            }
            catch (...)
            {}
        }
    }
    FreeLibraryAndExitThread(module, 0);
    return 0;
}
} // namespace

bool RequestKindCapture(RequestKind kind)
{
    auto& registry = GetRegistry(kind);
    std::lock_guard lock(registry.mutex);
    if (registry.status.attempted || registry.requested.load(std::memory_order_relaxed))
        return false;
    registry.status = { .queued = true, .busy = true,
        .message = kind == RequestKind::FogLayers ? "Queued: waiting for authenticated fog-layer snapshots." :
                                                   "Queued: waiting for authenticated private early guides.",
        .kind = kind == RequestKind::FogLayers ? "fog_layers" : "early_guides" };
    registry.requested.store(true, std::memory_order_release);
    return true;
}

void CancelKindCapture(RequestKind kind)
{
    auto& registry = GetRegistry(kind);
    std::lock_guard lock(registry.mutex);
    if (!registry.requested.exchange(false, std::memory_order_acq_rel))
        return;
    registry.status = { .message = "Queued native capture cancelled." };
}

bool Request() { return RequestKindCapture(RequestKind::FogLayers); }
bool RequestEarlyGuides() { return RequestKindCapture(RequestKind::EarlyGuides); }
void CancelRequest() { CancelKindCapture(RequestKind::FogLayers); }
void CancelEarlyGuideRequest() { CancelKindCapture(RequestKind::EarlyGuides); }

bool WantsCapture()
{
    auto& registry = GetRegistry(RequestKind::FogLayers);
    return registry.requested.load(std::memory_order_acquire);
}

bool WantsEarlyGuideCapture()
{
    auto& registry = GetRegistry(RequestKind::EarlyGuides);
    return registry.requested.load(std::memory_order_acquire);
}

Status GetStatus()
{
    auto& registry = GetRegistry(RequestKind::FogLayers);
    std::lock_guard lock(registry.mutex);
    return registry.status;
}

Status GetEarlyGuideStatus()
{
    auto& registry = GetRegistry(RequestKind::EarlyGuides);
    std::lock_guard lock(registry.mutex);
    return registry.status;
}

bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Layers& layers,
            const std::string& provenanceJson, const std::shared_ptr<void>& keepAlive)
{
    auto& registry = GetRegistry(RequestKind::FogLayers);
    {
        std::lock_guard lock(registry.mutex);
        if (!registry.requested.exchange(false, std::memory_order_acq_rel))
            return false;
        registry.status.queued = false;
        registry.status.attempted = true;
        registry.status.message = "Preparing native fog-layer readback.";
    }

    HMODULE module = nullptr;
    bool recorded = false;
    try
    {
        if (!device || !list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::runtime_error("fog capture requires a valid device and direct command list");
        CheckSameDevice(device, list);
        if (provenanceJson.size() > MaxProvenanceBytes)
            throw std::runtime_error("fog capture provenance exceeds the size limit");
        const auto provenance = Json::parse(provenanceJson);
        if (!provenance.is_object())
            throw std::runtime_error("fog capture provenance must be a JSON object");
        if (layers.before.resource.Get() == layers.after.resource.Get() ||
            layers.before.resource.Get() == layers.authored.resource.Get() ||
            layers.after.resource.Get() == layers.authored.resource.Get())
            throw std::runtime_error("fog capture requires three distinct immutable snapshot resources");
        if (layers.before.viewFormat != layers.after.viewFormat)
            throw std::runtime_error("fog capture before/after must preserve the same native scene view format");

        auto batch = std::make_shared<Batch>();
        batch->entries = { Entry { .role = "scene_before", .source = layers.before },
                           Entry { .role = "scene_after", .source = layers.after },
                           Entry { .role = "authored_fog", .source = layers.authored } };
        batch->keepAlive = keepAlive;
        UINT64 totalBytes = 0;
        for (auto& entry : batch->entries)
        {
            PrepareEntry(device, entry);
            totalBytes += entry.bytes;
            if (totalBytes > MaxBytes)
                throw std::runtime_error("fog capture total readback exceeds the 256 MiB limit");
            const auto& first = batch->entries.front().footprint.Footprint;
            if (entry.footprint.Footprint.Width != first.Width || entry.footprint.Footprint.Height != first.Height)
                throw std::runtime_error("fog capture layers must have matching subresource dimensions");
        }
        if (layers.boundCb12.resource)
        {
            for (const auto& entry : batch->entries)
                if (entry.source.resource.Get() == layers.boundCb12.resource.Get())
                    throw std::runtime_error("bound cb12 companion must be distinct from all scene layers");
            batch->boundCb12 = Entry { .role = "bound_cb12_words", .source = layers.boundCb12 };
            PrepareEntry(device, *batch->boundCb12, true);
            totalBytes += batch->boundCb12->bytes;
            if (totalBytes > MaxBytes)
                throw std::runtime_error("fog capture including bound cb12 exceeds the 256 MiB readback limit");
        }
        const auto guideCount = std::count_if(layers.earlyGuides.begin(), layers.earlyGuides.end(),
                                              [](const auto& texture) { return bool(texture.resource); });
        if (guideCount && guideCount != 3)
            throw std::runtime_error("early guide companions must be all present or all absent");
        if (guideCount)
        {
            constexpr const char* roles[] = { "early_diffuse_albedo", "early_specular_albedo", "early_normal_roughness" };
            std::array<Entry, 3> guides;
            for (size_t i = 0; i < guides.size(); ++i)
            {
                const auto& texture = layers.earlyGuides[i];
                for (const auto& entry : batch->entries)
                    if (texture.resource.Get() == entry.source.resource.Get())
                        throw std::runtime_error("early guide must not alias a scene layer");
                if (texture.resource.Get() == layers.boundCb12.resource.Get())
                    throw std::runtime_error("early guide must not alias the bound constant companion");
                for (size_t previous = 0; previous < i; ++previous)
                    if (texture.resource.Get() == layers.earlyGuides[previous].resource.Get())
                        throw std::runtime_error("early guide companions must be distinct");
                const auto desc = texture.resource->GetDesc();
                const auto format = i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
                const auto& scene = batch->entries.front().footprint.Footprint;
                if (texture.subresource != 0 || texture.viewFormat != format || desc.Format != format ||
                    desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Quality != 0 ||
                    desc.Width != scene.Width || desc.Height != scene.Height)
                    throw std::runtime_error("early guide companion layout does not match its native contract");
                guides[i] = Entry { .role = roles[i], .source = texture };
                PrepareEntry(device, guides[i], false, i < 2);
                totalBytes += guides[i].bytes;
                if (totalBytes > MaxBytes)
                    throw std::runtime_error("fog capture including early guides exceeds the 256 MiB readback limit");
            }
            batch->earlyGuides = std::move(guides);
        }
        if (layers.hardwareDepth.resource)
        {
            if (!keepAlive)
                throw std::runtime_error("hardware depth companion requires its retained private snapshot owner");
            ComPtr<IUnknown> depthIdentity;
            Check(layers.hardwareDepth.resource->QueryInterface(IID_PPV_ARGS(&depthIdentity)),
                  "hardware depth identity unavailable");
            if (!depthIdentity)
                throw std::runtime_error("hardware depth identity is null");
            const auto distinct = [&](const Entry& entry) {
                ComPtr<IUnknown> identity;
                Check(entry.source.resource->QueryInterface(IID_PPV_ARGS(&identity)),
                      "fog companion identity unavailable");
                if (!identity || identity.Get() == depthIdentity.Get())
                    throw std::runtime_error("hardware depth must not alias a scene layer or companion");
            };
            for (const auto& entry : batch->entries) distinct(entry);
            if (batch->boundCb12) distinct(*batch->boundCb12);
            if (batch->earlyGuides)
                for (const auto& entry : *batch->earlyGuides) distinct(entry);
            batch->hardwareDepth = Entry { .role = "hardware_depth", .source = layers.hardwareDepth };
            // Reuse the already narrow PRIVATE R32 role validator/raw writer;
            // this does not make any ordinary fog layer accept scalar textures.
            PrepareEntry(device, *batch->hardwareDepth, false, false, false, true);
            const auto& scene = batch->entries.front().footprint.Footprint;
            const auto& depth = batch->hardwareDepth->footprint.Footprint;
            if (depth.Width != scene.Width || depth.Height != scene.Height)
                throw std::runtime_error("hardware depth companion must match the captured scene extent");
            totalBytes += batch->hardwareDepth->bytes;
            if (totalBytes > MaxBytes)
                throw std::runtime_error("fog capture including hardware depth exceeds the shared256MiB readback limit");
        }
        const auto privateResetCount = std::count_if(layers.privateReset.begin(), layers.privateReset.end(),
                                                      [](const auto& texture) { return bool(texture.resource); });
        if (privateResetCount && privateResetCount != 3)
            throw std::runtime_error("private RESET outputs must be all present or all absent");
        if (layers.privateResetSceneWrite && (privateResetCount != 3 || layers.rgbIdentity.resource))
            throw std::runtime_error("one-shot RESET scene metadata requires all three outputs and no RGB identity companion");
        if (privateResetCount)
        {
            if (!keepAlive)
                throw std::runtime_error("private RESET outputs require retained recording owners");
            constexpr const char* roles[] = {
                "private_reset_radiance", "private_reset_denoised", "private_reset_composed"
            };
            std::array<Entry, 3> outputs;
            std::array<ComPtr<IUnknown>, 3> identities;
            const auto& scene = batch->entries.front().footprint.Footprint;
            for (size_t i = 0; i < outputs.size(); ++i)
            {
                const auto& texture = layers.privateReset[i];
                const auto desc = texture.resource->GetDesc();
                // Reuse the exact typed RGBA16F private-copy layout/state gate,
                // not its ray provenance or semantics. No format broadening.
                if (!IsRayCopyTexture(desc, texture.viewFormat, texture.subresource, texture.state,
                                      scene.Width, scene.Height, false))
                    throw std::runtime_error("private RESET output layout/state must match the native scene extent");
                Check(texture.resource->QueryInterface(IID_PPV_ARGS(&identities[i])),
                      "private RESET output identity unavailable");
                if (!identities[i])
                    throw std::runtime_error("private RESET output identity is null");
                const auto distinct = [&](const Entry& entry) {
                    ComPtr<IUnknown> identity;
                    Check(entry.source.resource->QueryInterface(IID_PPV_ARGS(&identity)),
                          "private RESET companion identity unavailable");
                    if (!identity || identity.Get() == identities[i].Get())
                        throw std::runtime_error("private RESET output must not alias another layer or companion");
                };
                for (const auto& entry : batch->entries) distinct(entry);
                if (batch->boundCb12) distinct(*batch->boundCb12);
                if (batch->earlyGuides)
                    for (const auto& entry : *batch->earlyGuides) distinct(entry);
                if (batch->hardwareDepth) distinct(*batch->hardwareDepth);
                if (batch->exposureWords) distinct(*batch->exposureWords);
                if (batch->lightingT8) distinct(*batch->lightingT8);
                if (batch->rayCopies)
                    for (const auto& entry : *batch->rayCopies) distinct(entry);
                for (size_t previous = 0; previous < i; ++previous)
                    if (identities[previous].Get() == identities[i].Get())
                        throw std::runtime_error("private RESET outputs must have distinct canonical identities");
                outputs[i] = Entry { .role = roles[i], .source = texture };
                PrepareEntry(device, outputs[i]);
                totalBytes += outputs[i].bytes;
                if (totalBytes > MaxBytes)
                    throw std::runtime_error("private RESET outputs exceed the shared256MiB readback limit");
            }
            batch->privateReset = std::move(outputs);
        }
        if (layers.rgbIdentity.resource)
        {
            if (!keepAlive)
                throw std::runtime_error("RGB identity snapshot requires retained recording owners");
            const auto& texture = layers.rgbIdentity;
            const auto& scene = batch->entries.front().footprint.Footprint;
            // Layout/state reuse only: this companion is not a ray-copy signal.
            if (!IsRayCopyTexture(texture.resource->GetDesc(), texture.viewFormat, texture.subresource,
                                  texture.state, scene.Width, scene.Height, false))
                throw std::runtime_error("RGB identity snapshot must be exact private RGBA16F/state0xc0 at scene extent");
            ComPtr<IUnknown> snapshotIdentity;
            Check(texture.resource->QueryInterface(IID_PPV_ARGS(&snapshotIdentity)),
                  "RGB identity snapshot resource identity unavailable");
            if (!snapshotIdentity)
                throw std::runtime_error("RGB identity snapshot resource identity is null");
            const auto distinct = [&](const Entry& entry) {
                ComPtr<IUnknown> identity;
                Check(entry.source.resource->QueryInterface(IID_PPV_ARGS(&identity)),
                      "RGB identity companion resource identity unavailable");
                if (!identity || identity.Get() == snapshotIdentity.Get())
                    throw std::runtime_error("RGB identity snapshot must not alias any layer or companion");
            };
            for (const auto& entry : batch->entries) distinct(entry);
            if (batch->boundCb12) distinct(*batch->boundCb12);
            if (batch->earlyGuides)
                for (const auto& entry : *batch->earlyGuides) distinct(entry);
            if (batch->hardwareDepth) distinct(*batch->hardwareDepth);
            if (batch->exposureWords) distinct(*batch->exposureWords);
            if (batch->lightingT8) distinct(*batch->lightingT8);
            if (batch->rayCopies)
                for (const auto& entry : *batch->rayCopies) distinct(entry);
            if (batch->privateReset)
                for (const auto& entry : *batch->privateReset) distinct(entry);
            batch->rgbIdentity = Entry { .role = "rgb_identity", .source = texture };
            PrepareEntry(device, *batch->rgbIdentity);
            totalBytes += batch->rgbIdentity->bytes;
            if (totalBytes > MaxBytes)
                throw std::runtime_error("RGB identity snapshot exceeds the shared256MiB readback limit");
        }
        // Validate every optional entry and the aggregate budget before allocating.
        for (auto& entry : batch->entries) AllocateReadback(device, entry);
        if (batch->boundCb12) AllocateReadback(device, *batch->boundCb12);
        if (batch->earlyGuides)
            for (auto& entry : *batch->earlyGuides) AllocateReadback(device, entry);
        if (batch->hardwareDepth) AllocateReadback(device, *batch->hardwareDepth);
        if (batch->privateReset)
            for (auto& entry : *batch->privateReset) AllocateReadback(device, entry);
        if (batch->rgbIdentity) AllocateReadback(device, *batch->rgbIdentity);
        batch->directory = Util::ExePath().parent_path() / "FSRRR-fog-captures" / Timestamp();
        batch->metadata = { { "schema", "optiscaler.fsr_rr.fog_layers.v1" },
                            { "complete", false },
                            { "process_id", GetCurrentProcessId() },
                            { "capture", batch->directory.filename().string() },
                            { "readback_bytes", totalBytes },
                            { "value_transform", "none; native float16/float32 RGBA including unmodified alpha" },
                            { "provenance_authority", "caller_supplied; draw authenticity not verified by readback helper" },
                            { "provenance", provenance },
                            { "layers", Json::array() }, { "companions", Json::array() } };
        for (const auto& entry : batch->entries)
            batch->metadata["layers"].push_back(Describe(entry));
        if (batch->boundCb12)
        {
            auto companion = Describe(*batch->boundCb12);
            companion["schema"] = "optiscaler.fsr_rr.bound_cb12_words.v1";
            companion["cb_register"] = 12;
            companion["register_space"] = 0;
            companion["register_indices"] = { 21, 22, 23, 24, 27 };
            companion["value_transform"] = "none; raw uint32 constant bits, no floating-point conversion";
            batch->metadata["companions"].push_back(std::move(companion));
        }
        if (batch->earlyGuides)
            for (size_t i = 0; i < batch->earlyGuides->size(); ++i)
            {
                auto companion = Describe((*batch->earlyGuides)[i]);
                companion["schema"] = "optiscaler.fsr_rr.early_guide.v1";
                companion["uav_register"] = i;
                companion["register_space"] = 0;
                companion["value_transform"] = "none; native authored guide output bytes";
                companion["input_provenance"] = "caller_supplied; not authenticated by readback helper";
                batch->metadata["companions"].push_back(std::move(companion));
            }
        if (batch->hardwareDepth)
        {
            auto companion = Describe(*batch->hardwareDepth);
            companion["schema"] = "optiscaler.fsr_rr.fog_hardware_depth.v1";
            companion["snapshot_stage"] = "original_fog_depth_use; caller_supplied";
            companion["value_transform"] = "none; native hardware-depth R float32 bits, no linearization";
            companion["input_provenance"] = "caller_supplied; original graph, plane, binding and snapshot order not authenticated by readback helper";
            companion["depth_convention"] = "caller must prove projection and depth encoding; not inferred from values or format";
            companion["cross_capture_frame_relation"] = "not_asserted";
            batch->metadata["companions"].push_back(std::move(companion));
        }

        if (batch->privateReset)
            for (size_t i = 0; i < batch->privateReset->size(); ++i)
            {
                auto companion = Describe((*batch->privateReset)[i]);
                companion["schema"] = "optiscaler.fsr_rr.private_reset_output.v1";
                companion["output_index"] = i;
                companion["intended_mode"] = "independent_RESET_diagnostic";
                companion["live_correction"] = false;
                if (layers.privateResetSceneWrite)
                {
                    companion["intended_mode"] = "independent_RESET_scene_control";
                    companion["live_correction"] = true;
                    companion["scene_write_scope"] = "one_shot_only; caller_supplied";
                    companion["scene_write_proof"] = "not_verified_by_readback_helper";
                }
                companion["value_transform"] = "none; native private RGBA float16 bits including unmodified alpha";
                companion["input_provenance"] = "caller_supplied; provider context, RESET, current inputs and frame association not authenticated by readback helper";
                batch->metadata["companions"].push_back(std::move(companion));
            }

        if (batch->rgbIdentity)
        {
            auto companion = Describe(*batch->rgbIdentity);
            companion["schema"] = "optiscaler.fsr_rr.fog_rgb_identity.v1";
            companion["snapshot_stage"] = "pre_fog_after_identity_rgb; caller_supplied";
            companion["identity_passed"] = false;
            companion["identity_validation"] = "not_performed; native comparison required";
            companion["value_transform"] = "none; native RGBA float16 bits including unmodified alpha";
            companion["input_provenance"] = "caller_supplied; identity draw, bindings and snapshot order not authenticated by readback helper";
            batch->metadata["companions"].push_back(std::move(companion));
        }

        auto args = std::make_unique<WorkerArgs>();
        args->batch = batch;
        // Hold a module reference only while worker code may execute; do not permanently pin it.
        static const char moduleMarker = 0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&moduleMarker), &module))
            throw std::runtime_error("fog capture could not retain its worker module");
        args->module = module;

        // Retain before the first command references any helper-owned resource. The ticket is
        // shared with other retained work on this recording and signaled by the existing hook.
        args->ticket = FSRDSubmission::Retain(device, list, batch);
        if (!args->ticket)
            throw std::runtime_error("fog capture could not retain its recording owners");
        {
            std::lock_guard lock(registry.mutex);
            registry.pending = batch;
            registry.ticket = args->ticket;
            registry.status.directory = batch->directory.string();
            registry.status.message = "Readback queued; waiting asynchronously for GPU submission/completion.";
        }
        for (const auto& entry : batch->entries)
            RecordCopy(list, entry);
        if (batch->boundCb12)
            RecordCopy(list, *batch->boundCb12);
        if (batch->earlyGuides)
            for (const auto& entry : *batch->earlyGuides)
                RecordCopy(list, entry);
        if (batch->hardwareDepth)
            RecordCopy(list, *batch->hardwareDepth);
        if (batch->privateReset)
            for (const auto& entry : *batch->privateReset)
                RecordCopy(list, entry);
        if (batch->rgbIdentity)
            RecordCopy(list, *batch->rgbIdentity);
        recorded = true;

        const auto thread = CreateThread(nullptr, 0, WriterThread, args.get(), 0, nullptr);
        if (!thread)
            throw std::runtime_error("fog capture could not start its background writer");
        args.release();
        module = nullptr; // Worker now releases the retained module reference.
        CloseHandle(thread);
        return true;
    }
    catch (const std::exception& error)
    {
        if (module)
            FreeLibrary(module);
        FinishStatus(RequestKind::FogLayers, false, std::format("Fog capture failed: {}{}", error.what(),
                                       recorded ? "; recorded GPU storage retained" : ""), false);
        return recorded;
    }
}

bool RecordEarlyGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                       const std::array<Texture, 3>& guides, const std::string& provenanceJson,
                       const std::shared_ptr<void>& keepAlive, const Texture* exposureWords,
                       const Texture* lightingT8, const std::array<Texture, 2>* rayCopies) noexcept
{
    HMODULE module = nullptr;
    bool recorded = false;
    bool claimed = false;
    try
    {
        auto& registry = GetRegistry(RequestKind::EarlyGuides);
        {
            std::lock_guard lock(registry.mutex);
            if (!registry.requested.exchange(false, std::memory_order_acq_rel))
                return false;
            claimed = true;
            registry.status.queued = false;
            registry.status.attempted = true;
            registry.status.message = "Preparing standalone native early-guide readback.";
        }
        if (!device || !list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !keepAlive)
            throw std::runtime_error("early guide capture requires direct list/device and retained dispatch owner");
        CheckSameDevice(device, list);
        if (provenanceJson.size() > MaxProvenanceBytes)
            throw std::runtime_error("early guide capture provenance exceeds the size limit");
        const auto provenance = Json::parse(provenanceJson);
        if (!provenance.is_object())
            throw std::runtime_error("early guide capture provenance must be a JSON object");

        auto batch = std::make_shared<Batch>();
        batch->guidesOnly = true;
        batch->keepAlive = keepAlive;
        constexpr const char* roles[] = { "early_diffuse_albedo", "early_specular_albedo", "early_normal_roughness" };
        constexpr D3D12_RESOURCE_STATES GuideState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        std::array<ComPtr<IUnknown>, 3> identities;
        ComPtr<IUnknown> exposureIdentity;
        ComPtr<IUnknown> inputIdentity;
        UINT64 totalBytes = 0;
        for (size_t i = 0; i < guides.size(); ++i)
        {
            const auto& texture = guides[i];
            if (!texture.resource)
                throw std::runtime_error("standalone early guide capture requires all three private outputs");
            Check(texture.resource->QueryInterface(IID_PPV_ARGS(&identities[i])), "guide resource identity unavailable");
            for (size_t previous = 0; previous < i; ++previous)
                if (identities[i].Get() == identities[previous].Get())
                    throw std::runtime_error("standalone early guides must have distinct canonical resource identities");
            const auto desc = texture.resource->GetDesc();
            const auto format = i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (texture.subresource != 0 || texture.viewFormat != format || desc.Format != format ||
                desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
                desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
                texture.state != GuideState)
                throw std::runtime_error("standalone early guide layout/state does not match the native private-pass contract");
            auto& entry = batch->entries[i];
            entry = Entry { .role = roles[i], .source = texture };
            PrepareEntry(device, entry, false, i < 2);
            const auto& first = batch->entries.front().footprint.Footprint;
            if (entry.footprint.Footprint.Width != first.Width || entry.footprint.Footprint.Height != first.Height)
                throw std::runtime_error("standalone early guides must have identical active extents");
            if (entry.bytes > MaxBytes - totalBytes)
                throw std::runtime_error("standalone early guide readback exceeds the shared 256 MiB limit");
            totalBytes += entry.bytes;
        }
        if (exposureWords)
        {
            if (!exposureWords->resource)
                throw std::runtime_error("supplied exposure-word companion must contain a private resource");
            Check(exposureWords->resource->QueryInterface(IID_PPV_ARGS(&exposureIdentity)),
                  "exposure-word resource identity unavailable");
            for (const auto& identity : identities)
                if (exposureIdentity.Get() == identity.Get())
                    throw std::runtime_error("exposure words must be distinct from all three native guides");
            batch->exposureWords = Entry { .role = "exposure_words", .source = *exposureWords };
            PrepareEntry(device, *batch->exposureWords, false, false, true);
            if (batch->exposureWords->bytes > MaxBytes - totalBytes)
                throw std::runtime_error("guides and exposure words exceed the shared256MiB readback limit");
            totalBytes += batch->exposureWords->bytes;
        }
        if (lightingT8)
        {
            if (!lightingT8->resource)
                throw std::runtime_error("supplied lighting t8 companion must contain an owned source resource");
            Check(lightingT8->resource->QueryInterface(IID_PPV_ARGS(&inputIdentity)),
                  "lighting t8 resource identity unavailable");
            for (const auto& identity : identities)
                if (inputIdentity.Get() == identity.Get())
                    throw std::runtime_error("lighting t8 must be distinct from all three native guides");
            if (exposureIdentity && inputIdentity.Get() == exposureIdentity.Get())
                throw std::runtime_error("lighting t8 must not alias the exposure-word companion");
            const auto& first = batch->entries.front().footprint.Footprint;
            if (!IsLightingT8Texture(lightingT8->resource->GetDesc(), lightingT8->viewFormat,
                                    lightingT8->subresource, lightingT8->state, first.Width, first.Height))
                throw std::runtime_error("lighting t8 requires owned render-sized mip0 RGBA16_FLOAT with requested read mask0x8c0");
            batch->lightingT8 = Entry { .role = "lighting_t8", .source = *lightingT8 };
            PrepareEntry(device, *batch->lightingT8);
            if (batch->lightingT8->bytes > MaxBytes - totalBytes)
                throw std::runtime_error("guides and optional lighting companions exceed the shared256MiB readback limit");
            totalBytes += batch->lightingT8->bytes;
        }
        if (rayCopies)
        {
            constexpr const char* rayRoles[] = { "ray_motion", "ray_hit" };
            std::array<ComPtr<IUnknown>, 2> rayIdentities;
            batch->rayCopies.emplace();
            for (size_t i = 0; i < rayCopies->size(); ++i)
            {
                const auto& texture = (*rayCopies)[i];
                if (!texture.resource)
                    throw std::runtime_error("ray snapshot companions require both private resources");
                Check(texture.resource->QueryInterface(IID_PPV_ARGS(&rayIdentities[i])),
                      "ray snapshot resource identity unavailable");
                for (const auto& identity : identities)
                    if (rayIdentities[i].Get() == identity.Get())
                        throw std::runtime_error("ray snapshots must not alias any native guide");
                if ((exposureIdentity && rayIdentities[i].Get() == exposureIdentity.Get()) ||
                    (inputIdentity && rayIdentities[i].Get() == inputIdentity.Get()))
                    throw std::runtime_error("ray snapshots must not alias exposure or lighting companions");
                for (size_t previous = 0; previous < i; ++previous)
                    if (rayIdentities[i].Get() == rayIdentities[previous].Get())
                        throw std::runtime_error("ray snapshots require distinct canonical resource identities");
                const auto& first = batch->entries.front().footprint.Footprint;
                if (!IsRayCopyTexture(texture.resource->GetDesc(), texture.viewFormat, texture.subresource,
                                      texture.state, first.Width, first.Height, i == 1))
                    throw std::runtime_error("ray snapshots require active-sized typed RGBA16F/R32F in state0xc0");
                auto& entry = (*batch->rayCopies)[i];
                entry = Entry { .role = rayRoles[i], .source = texture };
                PrepareEntry(device, entry, false, false, false, i == 1);
                if (entry.bytes > MaxBytes - totalBytes)
                    throw std::runtime_error("guides and optional ray companions exceed the shared256MiB readback limit");
                totalBytes += entry.bytes;
            }
        }
        for (auto& entry : batch->entries)
            AllocateReadback(device, entry);
        if (batch->exposureWords)
            AllocateReadback(device, *batch->exposureWords);
        if (batch->lightingT8)
            AllocateReadback(device, *batch->lightingT8);
        if (batch->rayCopies)
            for (auto& entry : *batch->rayCopies)
                AllocateReadback(device, entry);

        batch->directory = Util::ExePath().parent_path() / "FSRRR-early-guide-captures" / Timestamp("early-guides");
        const auto& extent = batch->entries.front().footprint.Footprint;
        batch->metadata = { { "schema", "optiscaler.fsr_rr.early_guide_capture.v1" },
            { "complete", false }, { "process_id", GetCurrentProcessId() },
            { "capture", batch->directory.filename().string() }, { "readback_bytes", totalBytes },
            { "render_extent", { extent.Width, extent.Height } },
            { "value_transform", "none; native authored RGBA8_UNORM/RGBA16F guide output bytes" },
            { "provenance_authority", "caller_supplied; source authenticity and frame association not verified by readback helper" },
            { "completion_authority", "existing same-list submission observer and GPU fence, then all native files written" },
            { "provenance", provenance }, { "companions", Json::array() } };
        for (size_t i = 0; i < batch->entries.size(); ++i)
        {
            auto companion = Describe(batch->entries[i]);
            companion["schema"] = "optiscaler.fsr_rr.early_guide.v1";
            companion["uav_register"] = i;
            companion["register_space"] = 0;
            companion["value_transform"] = "none; native authored guide output bytes";
            companion["input_provenance"] = "caller_supplied; not authenticated by readback helper";
            batch->metadata["companions"].push_back(std::move(companion));
        }
        if (batch->exposureWords)
        {
            auto companion = Describe(*batch->exposureWords);
            companion["schema"] = "optiscaler.fsr_rr.exposure_words.v1";
            companion["word_layout"] = "row-major RGBA uint32 words0..6 then one zero pad";
            companion["source_word_byte_offsets"] = { 0, 4, 8, 12, 16, 20, 24 };
            companion["source_word_count"] = 7;
            companion["padding_word_index"] = 7;
            companion["padding_word_value"] = 0;
            companion["value_transform"] = "none; raw uint32 bits, no float conversion or exposure normalization";
            companion["input_provenance"] = "caller_supplied; actual GPU-word producer and binding not authenticated by readback helper";
            batch->metadata["companions"].push_back(std::move(companion));
        }
        if (batch->lightingT8)
        {
            auto companion = Describe(*batch->lightingT8);
            companion.erase("state_restored");
            companion["schema"] = "optiscaler.fsr_rr.lighting_t8.v1";
            companion["required_read_state"] = 0x8c0;
            companion["source_state_authority"] = "engine-managed; required read bits established by caller, exact native state mask not established";
            companion["input_barriers_recorded"] = false;
            companion["shader_stage"] = "pixel";
            companion["shader_register"] = 8;
            companion["register_space"] = 0;
            companion["signal_semantics"] = "raw encoded original-lighting input; not established as raw-ray or undenoised radiance";
            companion["value_transform"] = "none; native encoded original-lighting input RGBA16F bytes";
            companion["input_provenance"] = "caller_supplied; original binding, ownership, required source read bits and frame association not authenticated by readback helper";
            batch->metadata["companions"].push_back(std::move(companion));
        }
        if (batch->rayCopies)
        {
            for (size_t i = 0; i < batch->rayCopies->size(); ++i)
            {
                auto companion = Describe((*batch->rayCopies)[i]);
                companion["schema"] = "optiscaler.fsr_rr.private_ray_copy.v1";
                companion["snapshot_stage"] = "original_ray_dispatch; caller_supplied";
                companion["source_role"] = i == 0 ? "motion" : "hit";
                companion["value_transform"] = "none; native byte copy, all channels and bits preserved";
                companion["semantic_units"] = "caller must prove motion scale and hit encoding; not inferred from format";
                companion["final_write_authority"] = "caller must prove selected writer and any final-write claim; not established by readback completion";
                companion["input_provenance"] = "caller_supplied; original ray binding, producer ordering and frame association not authenticated by readback helper";
                batch->metadata["companions"].push_back(std::move(companion));
            }
        }

        auto args = std::make_unique<WorkerArgs>();
        args->batch = batch;
        static const char moduleMarker = 0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&moduleMarker), &module))
            throw std::runtime_error("early guide capture could not retain its worker module");
        args->module = module;
        // Same native-list observer as fog/RR capture. No callback/frame-order
        // heuristic substitutes for observed submission and successful fencing.
        args->ticket = FSRDSubmission::Retain(device, list, batch);
        if (!args->ticket)
            throw std::runtime_error("early guide capture could not retain its readback batch");
        {
            std::lock_guard lock(registry.mutex);
            registry.pending = batch;
            registry.ticket = args->ticket;
            registry.status.directory = batch->directory.string();
            registry.status.message = "Early-guide readback queued; waiting asynchronously for GPU submission/completion.";
        }
        for (const auto& entry : batch->entries)
            RecordCopy(list, entry);
        if (batch->exposureWords)
            RecordCopy(list, *batch->exposureWords);
        if (batch->lightingT8)
            RecordCopy(list, *batch->lightingT8);
        if (batch->rayCopies)
            for (const auto& entry : *batch->rayCopies)
                RecordCopy(list, entry);
        recorded = true;

        const auto thread = CreateThread(nullptr, 0, WriterThread, args.get(), 0, nullptr);
        if (!thread)
            throw std::runtime_error("early guide capture could not start its background writer");
        args.release();
        module = nullptr;
        CloseHandle(thread);
        return true;
    }
    catch (...)
    {
        if (module) FreeLibrary(module);
        // No exception escapes the original scene hook, including logging/status
        // allocation failure. Retention already owns any partially recorded work.
        if (claimed)
            try
            {
                std::string reason = "unknown failure";
                try { throw; }
                catch (const std::exception& error) { reason = error.what(); }
                catch (...) {}
                FinishStatus(RequestKind::EarlyGuides, false, std::format("Early guide capture failed: {}{}", reason,
                             recorded ? "; readback recorded, GPU storage retained" : "; no completed readback batch"), false);
            }
            catch (...) {}
        return recorded;
    }
}
} // namespace FSRDFogLayerCapture

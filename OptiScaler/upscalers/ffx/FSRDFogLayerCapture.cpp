#include "pch.h"
#include "FSRDFogLayerCapture.h"
#include "resource_tracking/FSRDSubmission.h"
#include "Util.h"
#include <json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
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
};

struct Batch
{
    std::array<Entry, 3> entries;
    std::optional<Entry> boundCb12;
    std::optional<std::array<Entry, 3>> earlyGuides;
    std::shared_ptr<void> keepAlive;
    Json metadata;
    std::filesystem::path directory;
};

struct Registry
{
    std::mutex mutex;
    std::atomic<bool> requested { false };
    Status status { .message = "No fog capture requested." };
    // Ticket owns Batch through FSRDSubmission. Batch must NOT own Ticket.
    // These additional references survive worker errors/timeouts without a GPU wait.
    std::shared_ptr<Batch> pending;
    std::shared_ptr<FSRDSubmission::Ticket> ticket;
};

Registry& GetRegistry()
{
    // Deliberately bounded to one attempt. Abandoned/failed GPU work survives DLL
    // teardown instead of releasing resources still referenced by a command list.
    static auto* registry = new Registry;
    return *registry;
}

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::format("{} ({:08X})", operation, static_cast<UINT>(result)));
}

std::string Timestamp()
{
    SYSTEMTIME now {};
    GetSystemTime(&now);
    return std::format("fog-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}Z-{}", now.wYear, now.wMonth,
                       now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
}

void FinishStatus(bool complete, const std::string& message, bool releaseStorage)
{
    // COM-backed storage must be destroyed only after unlocking (Release can enter hooks).
    std::shared_ptr<Batch> retiredBatch;
    std::shared_ptr<FSRDSubmission::Ticket> retiredTicket;
    auto& registry = GetRegistry();
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

void PrepareEntry(ID3D12Device* device, Entry& entry, bool boundCb12 = false, bool guideAlbedo = false)
{
    if (!entry.source.resource)
        throw std::runtime_error("fog capture requires all three immutable textures");
    CheckSameDevice(device, entry.source.resource.Get());
    const auto desc = entry.source.resource->GetDesc();
    if (boundCb12)
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
             { "channels", "RGBA" },
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

void WriteEntry(const Batch& batch, const Entry& entry)
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

    const auto path = batch.directory / entry.filename;
    auto temporary = path;
    temporary += ".part";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    const auto* pixels = static_cast<const char*>(mapping.data) + entry.footprint.Offset;
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
    const auto start = GetTickCount64();
    while (!FSRDSubmission::Complete(args.ticket))
    {
        if (SubmissionFailed(args.ticket))
        {
            FinishStatus(false, "Fog capture submission failed/device removed; GPU storage retained.", false);
            return;
        }
        if (GetTickCount64() - start >= CompletionTimeoutMs)
        {
            FinishStatus(false, "Fog capture completion timed out; GPU storage retained (no render-thread wait).", false);
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
        WriteManifest(batch); // Explicitly incomplete until all three files are closed successfully.
        for (const auto& entry : batch.entries)
            WriteEntry(batch, entry);
        if (batch.boundCb12)
            WriteEntry(batch, *batch.boundCb12);
        if (batch.earlyGuides)
            for (const auto& entry : *batch.earlyGuides)
                WriteEntry(batch, entry);
        batch.metadata["complete"] = true;
        WriteManifest(batch);
        FinishStatus(true, "Fog capture saved: three native floating-point RGBA layers and provenance.", true);
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
        FinishStatus(false, std::format("Fog capture write failed: {}", error.what()), true);
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
                FinishStatus(false, "Fog capture worker failed; GPU storage retained.", false);
            }
            catch (...)
            {}
        }
    }
    FreeLibraryAndExitThread(module, 0);
    return 0;
}
} // namespace

bool Request()
{
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (registry.status.attempted || registry.requested.load(std::memory_order_relaxed))
        return false;
    registry.status = { .queued = true, .busy = true, .message = "Queued: waiting for authenticated fog-layer snapshots." };
    registry.requested.store(true, std::memory_order_release);
    return true;
}

void CancelRequest()
{
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (!registry.requested.exchange(false, std::memory_order_acq_rel))
        return;
    registry.status = { .message = "Queued fog capture cancelled." };
}

bool WantsCapture()
{
    return GetRegistry().requested.load(std::memory_order_acquire);
}

Status GetStatus()
{
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    return registry.status;
}

bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Layers& layers,
            const std::string& provenanceJson, const std::shared_ptr<void>& keepAlive)
{
    auto& registry = GetRegistry();
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
        for (auto& entry : batch->entries)
            AllocateReadback(device, entry);
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
            AllocateReadback(device, *batch->boundCb12);
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
            for (auto& entry : guides)
                AllocateReadback(device, entry);
            batch->earlyGuides = std::move(guides);
        }
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
        FinishStatus(false, std::format("Fog capture failed: {}{}", error.what(),
                                       recorded ? "; recorded GPU storage retained" : ""), false);
        return recorded;
    }
}
} // namespace FSRDFogLayerCapture

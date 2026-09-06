// Offline diagnostic only: one captured frame, one fresh-context RESET dispatch.
// No injection, window, swapchain, game configuration, or OptiScaler dependencies.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <ffx_denoiser.h>
#include <dx12/ffx_api_dx12.h>
#include <json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
namespace fs = std::filesystem;

// The pinned API header declares exported entry points. Define all five explicitly;
// calls forward to the same standalone denoiser provider used by OptiScaler.
static PfnFfxCreateContext createFn;
static PfnFfxDestroyContext destroyFn;
static PfnFfxConfigure configureFn;
static PfnFfxQuery queryFn;
static PfnFfxDispatch dispatchFn;
extern "C" ffxReturnCode_t ffxCreateContext(ffxContext* c, ffxCreateContextDescHeader* d,
                                           const ffxAllocationCallbacks* a) { return createFn(c, d, a); }
extern "C" ffxReturnCode_t ffxDestroyContext(ffxContext* c, const ffxAllocationCallbacks* a) { return destroyFn(c, a); }
extern "C" ffxReturnCode_t ffxConfigure(ffxContext* c, const ffxConfigureDescHeader* d) { return configureFn(c, d); }
extern "C" ffxReturnCode_t ffxQuery(ffxContext* c, ffxQueryDescHeader* d) { return queryFn(c, d); }
extern "C" ffxReturnCode_t ffxDispatch(ffxContext* c, const ffxDispatchDescHeader* d) { return dispatchFn(c, d); }

static void check(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(uint32_t(result)));
}
static void checkFfx(ffxReturnCode_t result, const char* operation)
{
    if (result != FFX_API_RETURN_OK)
        throw std::runtime_error(std::string(operation) + " FFX return=" + std::to_string(result));
}
static void message(uint32_t type, const wchar_t* text)
{
    std::wcerr << L"AMD[" << type << L"] " << text << std::endl;
}
static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "Create buffer");
    return result;
}
static ComPtr<ID3D12Resource> texture(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
                                      D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&result)), "Create texture");
    return result;
}
static FfxApiFloatCoords3D vec3(const json& j)
{
    return { j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>() };
}

static int run(int argc, wchar_t** argv)
{
    if (argc != 4)
        throw std::runtime_error("Usage: fsrrr-replay.exe job.json absolute-provider.dll NEW-output-directory");
    fs::path jobPath = fs::absolute(argv[1]);
    fs::path providerPath = fs::absolute(argv[2]);
    fs::path outputPath = fs::absolute(argv[3]);
    json job = json::parse(std::ifstream(jobPath));
    if (job.at("schema") != 1 || fs::exists(outputPath))
        throw std::runtime_error("Unknown schema or output directory already exists");
    const auto& d = job.at("dispatch");
    uint32_t w = d.at("render_size").at(0), h = d.at("render_size").at(1);
    if (!w || !h || w > 8192 || h > 8192 || !(d.at("flags").get<uint32_t>() & FFX_DENOISER_DISPATCH_RESET))
        throw std::runtime_error("Replay requires valid dimensions and explicit RESET");
    // Validate paths and file sizes before loading a provider or creating GPU resources.
    std::map<std::string, std::vector<char>> bytes;
    std::map<std::string, std::pair<uint32_t, uint32_t>> resourceSizes;
    const std::map<std::string, uint32_t> expected {
        { "converted_radiance", 10 }, { "converted_motion", 10 }, { "converted_depth", 41 },
        { "converted_normals", 24 }, { "converted_diffuse_albedo", 24 },
        { "converted_specular_albedo", 24 }, { "converted_fused_albedo", 24 }
    };
    for (const auto& entry : job.at("textures"))
    {
        const auto name = entry.at("name").get<std::string>();
        const auto filename = fs::path(entry.at("file").get<std::string>());
        const auto fmt = entry.at("format").get<uint32_t>();
        if (!expected.contains(name) || expected.at(name) != fmt || bytes.contains(name) ||
            filename.has_parent_path() || filename.is_absolute() || filename.empty())
            throw std::runtime_error("Invalid or duplicate texture entry");
        const auto sizeJson = entry.value("resource_size", json::array({ w, h }));
        uint32_t resourceW = sizeJson.at(0), resourceH = sizeJson.at(1);
        if (resourceW < w || resourceH < h || resourceW > 8192 || resourceH > 8192)
            throw std::runtime_error("Invalid texture allocation dimensions");
        resourceSizes[name] = { resourceW, resourceH };
        size_t size = size_t(w) * h * (fmt == 10 ? 8 : 4);
        const auto path = jobPath.parent_path() / filename;
        if (fs::file_size(path) != size || entry.at("bytes").get<size_t>() != size)
            throw std::runtime_error("Incomplete texture: " + name);
        auto& data = bytes[name];
        data.resize(size);
        std::ifstream input(path, std::ios::binary);
        if (!input.read(data.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read texture: " + name);
    }
    if (bytes.size() != expected.size())
        throw std::runtime_error("Missing required texture");

    HMODULE module = LoadLibraryExW(providerPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        throw std::runtime_error("Load provider failed: " + std::to_string(GetLastError()));
    createFn = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
    destroyFn = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
    configureFn = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
    queryFn = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
    dispatchFn = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
    if (!createFn || !destroyFn || !configureFn || !queryFn || !dispatchFn)
        throw std::runtime_error("Provider entry points missing");

    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create factory");
    ComPtr<IDXGIAdapter1> selected;
    std::wstring wanted = fs::path(job.at("adapter").get<std::string>()).wstring();
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        auto hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        check(hr, "Enumerate adapter");
        DXGI_ADAPTER_DESC1 desc {};
        check(adapter->GetDesc1(&desc), "Adapter description");
        std::wcout << L"Adapter: " << desc.Description << std::endl;
        if (wanted == desc.Description && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            if (selected)
                throw std::runtime_error("Adapter name is ambiguous");
            selected = adapter;
        }
    }
    if (!selected)
        throw std::runtime_error("Requested adapter not found; refusing automatic GPU fallback");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "Create D3D12 device");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qdesc {};
    check(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "Create queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&list)), "Create command list");

    uint64_t count = 0;
    ffxQueryDescGetVersions versions {};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_EFFECT_ID_DENOISER;
    versions.device = device.Get();
    versions.outputCount = &count;
    checkFfx(ffxQuery(nullptr, &versions.header), "Query version count");
    if (count != 1)
        throw std::runtime_error("Expected exactly one provider; explicit selection needed otherwise");
    uint64_t id = 0;
    const char* versionName = nullptr;
    versions.versionIds = &id;
    versions.versionNames = &versionName;
    checkFfx(ffxQuery(nullptr, &versions.header), "Query version");
    job["provider_version"] = versionName ? versionName : "unknown";
    job["provider_id"] = id;
    std::cout << "Provider: " << job["provider_version"] << std::endl;
    ffxOverrideVersion version {};
    version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version.versionId = id;
    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.header.pNext = &version.header;
    backend.device = device.Get();
    ffxCreateContextDescDenoiser create {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    create.header.pNext = &backend.header;
    create.version = FFX_DENOISER_VERSION;
    create.mode = FFX_DENOISER_MODE_1_SIGNAL;
    create.maxRenderSize = { job.at("max_render_size").at(0).get<uint32_t>(),
                             job.at("max_render_size").at(1).get<uint32_t>() };
    if (create.maxRenderSize.width < w || create.maxRenderSize.height < h ||
        create.maxRenderSize.width > 8192 || create.maxRenderSize.height > 8192)
        throw std::runtime_error("Invalid context dimensions");
    create.fpMessage = message;
    ffxContext context = nullptr;
    checkFfx(ffxCreateContext(&context, &create.header, nullptr), "Create denoiser");
    for (uint64_t key = 1; key <= 6; ++key)
    {
        float value = 0;
        ffxQueryDescDenoiserGetDefaultKeyValue query {};
        query.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
        query.key = key;
        query.count = 1;
        query.data = &value;
        checkFfx(ffxQuery(&context, &query.header), "Query default setting");
        job["provider_defaults"][std::to_string(key)] = value;
    }

    constexpr auto readState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::map<std::string, ComPtr<ID3D12Resource>> inputs;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    for (const auto& [name, fmt] : expected)
    {
        auto& tex = inputs[name];
        const auto [resourceW, resourceH] = resourceSizes.at(name);
        tex = texture(device.Get(), resourceW, resourceH, DXGI_FORMAT(fmt), D3D12_RESOURCE_STATE_COPY_DEST);
        auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0;
        UINT64 rowSize = 0, total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
        auto upload = buffer(device.Get(), total, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        check(upload->Map(0, &noRead, &mapped), "Map upload");
        memset(mapped, 0, size_t(total));
        const size_t activeRowSize = size_t(w) * (fmt == 10 ? 8 : 4);
        for (UINT y = 0; y < h; ++y)
            memcpy(static_cast<char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                   bytes.at(name).data() + size_t(y) * activeRowSize, activeRowSize);
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readState);
        uploads.push_back(upload);
    }
    const auto outputSize = job.value("output_size", json::array({ w, h }));
    uint32_t outputW = outputSize.at(0), outputH = outputSize.at(1);
    if (outputW < w || outputH < h || outputW > 8192 || outputH > 8192)
        throw std::runtime_error("Invalid output allocation dimensions");
    auto output = texture(device.Get(), outputW, outputH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto resource = [&](const char* name) { return ffxApiGetResourceDX12(inputs.at(name).Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ); };
    ffxDispatchDescDenoiserInput1Signal signal {};
    signal.header.type = FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER;
    signal.radiance.input = resource("converted_radiance");
    signal.radiance.output = ffxApiGetResourceDX12(output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    signal.fusedAlbedo = resource("converted_fused_albedo");
    ffxDispatchDescDenoiser dispatch {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    dispatch.header.pNext = &signal.header;
    dispatch.commandList = list.Get();
    dispatch.linearDepth = resource("converted_depth");
    dispatch.motionVectors = resource("converted_motion");
    dispatch.normals = resource("converted_normals");
    dispatch.specularAlbedo = resource("converted_specular_albedo");
    dispatch.diffuseAlbedo = resource("converted_diffuse_albedo");
    dispatch.motionVectorScale = vec3(d.at("motion_scale"));
    dispatch.jitterOffsets = { d.at("jitter").at(0).get<float>(), d.at("jitter").at(1).get<float>() };
    dispatch.cameraPositionDelta = vec3(d.at("camera_delta"));
    dispatch.cameraRight = vec3(d.at("camera_right"));
    dispatch.cameraUp = vec3(d.at("camera_up"));
    dispatch.cameraForward = vec3(d.at("camera_forward"));
    dispatch.cameraAspectRatio = d.at("aspect");
    dispatch.cameraNear = d.at("near");
    dispatch.cameraFar = d.at("far");
    dispatch.cameraFovAngleVertical = d.at("fov");
    dispatch.renderSize = { w, h };
    dispatch.deltaTime = d.at("delta_ms");
    dispatch.frameIndex = d.at("frame_index");
    dispatch.flags = d.at("flags");
    checkFfx(ffxDispatch(&context, &dispatch.header), "Dispatch denoiser");
    barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto outputDesc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&outputDesc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readback = buffer(device.Get(), total, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = output.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "Close command list");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("Create fence event failed");
    check(fence->SetEventOnCompletion(1, event), "Set completion event");
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    // Never unwind/free resources while the GPU might still be using them.
    if (FAILED(queue->Signal(fence.Get(), 1)) || WaitForSingleObject(event, 45000) != WAIT_OBJECT_0 ||
        fence->GetCompletedValue() == UINT64_MAX || FAILED(device->GetDeviceRemovedReason()))
    {
        std::cerr << "GPU submission failed/timed out; exiting without live-resource teardown" << std::endl;
        ExitProcess(2);
    }
    CloseHandle(event);
    fs::create_directories(outputPath);
    void* mapped = nullptr;
    D3D12_RANGE range { 0, size_t(total) };
    check(readback->Map(0, &range, &mapped), "Map readback");
    std::ofstream result(outputPath / "denoised.rgba16f", std::ios::binary);
    for (uint32_t y = 0; y < h; ++y)
        result.write(static_cast<char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(w) * 8);
    D3D12_RANGE noWrite { 0, 0 };
    readback->Unmap(0, &noWrite);
    result.close();
    if (!result)
        throw std::runtime_error("Cannot write denoiser result");
    checkFfx(ffxDestroyContext(&context, nullptr), "Destroy denoiser");
    job["completed"] = true;
    job["output"] = "denoised.rgba16f";
    std::ofstream manifest(outputPath / "result.json");
    manifest << job.dump(2) << '\n';
    manifest.close();
    if (!manifest)
        throw std::runtime_error("Cannot write result manifest");
    std::cout << "Completed reset-frame replay" << std::endl;
    // Keep provider loaded through COM resource teardown.
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    try { return run(argc, argv); }
    catch (const std::exception& error)
    {
        std::cerr << "Replay failed: " << error.what() << std::endl;
        return 1;
    }
}

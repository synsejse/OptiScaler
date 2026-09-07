#include "pch.h"
#include "FSRDCyberpunkGuidePass.h"
#include "resource_tracking/FSRDSubmission.h"
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <atomic>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace FSRD::CyberpunkGuidePass
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr size_t ShaderBytes = 5824;
constexpr std::string_view ShaderSha256Text = "a4bcbce1667fb6f3e130a18482e6088e2835d0028582a52e5a4668f7fae1b7ee";
constexpr UINT SharedAlignedBytes = 1792;
constexpr UINT ConstantBytes = 2048;
static_assert(SharedAlignedBytes % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0 &&
              sizeof(CyberpunkGuideConstants::SharedConstants) <= SharedAlignedBytes &&
              SharedAlignedBytes + sizeof(CyberpunkGuideConstants::PassConstants) <= ConstantBytes);
constexpr UINT MaxDimension = 8192;
constexpr UINT64 MaxOutputBytes = 256ull * 1024 * 1024;
constexpr D3D12_RESOURCE_STATES Readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void Require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}
void Check(HRESULT result, const char* reason) { Require(SUCCEEDED(result), reason); }

// Load CNG only for explicitly prepared research work; no new ordinary-build DLL import.
bool Authenticate(std::span<const std::byte> bytes)
{
    if (bytes.size() != ShaderBytes || std::memcmp(bytes.data(), "DXBC", 4) != 0) return false;
    uint32_t declared = 0;
    std::memcpy(&declared, bytes.data() + 24, sizeof(declared));
    if (declared != bytes.size()) return false;
    const auto module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    struct ModuleOwner { HMODULE module; ~ModuleOwner() { FreeLibrary(module); } } moduleOwner { module };
    const auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(GetProcAddress(module, "BCryptOpenAlgorithmProvider"));
    const auto close = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(GetProcAddress(module, "BCryptCloseAlgorithmProvider"));
    const auto hash = reinterpret_cast<decltype(&BCryptHash)>(GetProcAddress(module, "BCryptHash"));
    if (!open || !close || !hash) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    struct AlgorithmOwner
    {
        BCRYPT_ALG_HANDLE algorithm;
        decltype(&BCryptCloseAlgorithmProvider) close;
        ~AlgorithmOwner() { close(algorithm, 0); }
    } algorithmOwner { algorithm, close };
    std::array<UCHAR, 32> digest {};
    if (hash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
             ULONG(bytes.size()), digest.data(), ULONG(digest.size())) < 0) return false;
    constexpr char hex[] = "0123456789abcdef";
    std::array<char, 64> text {};
    for (size_t i = 0; i < digest.size(); ++i)
    {
        text[i * 2] = hex[digest[i] >> 4];
        text[i * 2 + 1] = hex[digest[i] & 15];
    }
    return std::string_view(text.data(), text.size()) == ShaderSha256Text;
}

ComPtr<IUnknown> Identity(IUnknown* object)
{
    ComPtr<IUnknown> result;
    Check(object->QueryInterface(IID_PPV_ARGS(&result)), "object identity unavailable");
    return result;
}
bool OnDevice(ID3D12DeviceChild* object, ID3D12Device* expected)
{
    ComPtr<ID3D12Device> device;
    return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&device))) &&
           Identity(device.Get()).Get() == Identity(expected).Get();
}

void ValidateSource(ID3D12Device* device, const SourceView& source, UINT width, UINT height)
{
    Require(source.resource && source.heap && source.descriptor.ptr, "source view/ownership absent");
    Require(OnDevice(source.resource.Get(), device) && OnDevice(source.heap.Get(), device), "source device differs");
    const auto resource = source.resource->GetDesc();
    Require(resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && resource.Width >= width &&
            resource.Height >= height && resource.DepthOrArraySize == 1 && resource.MipLevels >= 1 &&
            resource.SampleDesc.Count == 1 && !(resource.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),
            "unsupported source allocation or extent");
    const auto heap = source.heap->GetDesc();
    Require(heap.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            heap.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE, "source descriptors must be CPU-only CSU");
    const auto begin = source.heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const auto stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    Require(stride && source.descriptor.ptr >= begin && (source.descriptor.ptr - begin) % stride == 0 &&
            (source.descriptor.ptr - begin) / stride < heap.NumDescriptors, "source descriptor outside retained heap");
}
} // namespace

struct Work::Impl
{
    ComPtr<ID3D12Device> device;
    std::array<SourceView, 4> sources;
    std::array<ComPtr<ID3D12Resource>, 3> outputs;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    UINT width = 0, height = 0, stride = 0;
    std::atomic<bool> attempted = false;
};

Work::Work(std::unique_ptr<Impl> implementation) : _impl(std::move(implementation)) {}
Work::~Work() = default;
const std::array<ComPtr<ID3D12Resource>, 3>& Work::Outputs() const { return _impl->outputs; }

std::shared_ptr<Work> Prepare(ID3D12Device* device, UINT width, UINT height,
    const std::array<SourceView, 4>& sources, const CyberpunkGuideConstants::PassConstants& pass,
    const CyberpunkGuideConstants::SharedConstants& shared, std::span<const std::byte> shader) noexcept
{
    try
    {
        Require(device && width && height && width <= MaxDimension && height <= MaxDimension, "invalid private guide extent");
        Require(pass[0] == std::bit_cast<uint32_t>(float(width)) && pass[1] == std::bit_cast<uint32_t>(float(height)),
                "b6 output extent mismatch");
        Require(pass[4] == 0 && pass[6] == 0, "active transparent/extra-specular branch requires unavailable inputs");
        CyberpunkGuideConstants::PassSources passSource;
        passSource.width = width; passSource.height = height;
        passSource.transparency = CyberpunkGuideConstants::TransparencyInput::PreTransparencySurface;
        passSource.noVMode = std::bit_cast<int32_t>(pass[5]);
        passSource.extraSpecularScaleBits = pass[7];
        CyberpunkGuideConstants::PassConstants checkedPass {};
        Require(CyberpunkGuideConstants::PackPass(passSource, checkedPass) && checkedPass == pass,
                "b6 does not match exact authored extent/reciprocal/branch packing");
        // Own the runtime input bytes before authenticating/creating the private PSO.
        Require(shader.size() == ShaderBytes, "unexpected guide shader size");
        const std::vector<std::byte> ownedShader(shader.begin(), shader.end());
        Require(Authenticate(ownedShader), "guide shader is not authenticated exact game bytecode");
        for (const auto& source : sources) ValidateSource(device, source, width, height);
        auto data = std::make_unique<Work::Impl>();
        data->device = device; data->sources = sources; data->width = width; data->height = height;
        data->stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ScopedSkipHeapCapture skipHeapCapture {};

        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[4] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 12;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister = 6;
        for (UINT i = 0; i < 2; ++i)
        {
            parameters[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i + 2].DescriptorTable = { 1, &ranges[i] };
        }
        D3D12_ROOT_SIGNATURE_DESC rootDescription {};
        rootDescription.NumParameters = 4; rootDescription.pParameters = parameters;
        ComPtr<ID3DBlob> serialized, errors;
        Check(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &serialized, &errors), "guide root serialization failed");
        Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&data->root)), "guide root creation failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
        pipeline.pRootSignature = data->root.Get();
        pipeline.CS = { ownedShader.data(), ownedShader.size() };
        Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&data->pipeline)), "guide pipeline creation failed");

        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 10; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&data->descriptors)), "guide descriptor allocation failed");
        const auto descriptorAt = [&](UINT index) {
            auto handle = data->descriptors->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(index) * data->stride; return handle;
        };
        constexpr UINT slots[] = { 0, 1, 2, 4 };
        for (UINT i = 0; i < sources.size(); ++i)
            device->CopyDescriptorsSimple(1, descriptorAt(slots[i]), sources[i].descriptor, heap.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture {};
        nullTexture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullTexture.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &nullTexture, descriptorAt(3));
        device->CreateShaderResourceView(nullptr, &nullTexture, descriptorAt(5));
        D3D12_SHADER_RESOURCE_VIEW_DESC nullExposure {};
        nullExposure.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        nullExposure.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullExposure.Buffer.NumElements = 1; nullExposure.Buffer.StructureByteStride = 28;
        device->CreateShaderResourceView(nullptr, &nullExposure, descriptorAt(6));

        D3D12_HEAP_PROPERTIES properties {}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC output {};
        output.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; output.Width = width; output.Height = height;
        output.DepthOrArraySize = 1; output.MipLevels = 1; output.SampleDesc.Count = 1;
        output.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        UINT64 allocated = 0;
        for (UINT i = 0; i < 3; ++i)
        {
            output.Format = i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
            const auto allocation = device->GetResourceAllocationInfo(0, 1, &output);
            Require(allocation.SizeInBytes && allocation.SizeInBytes <= MaxOutputBytes - allocated, "guide output allocation exceeds budget");
            allocated += allocation.SizeInBytes;
            Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &output,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&data->outputs[i])), "guide output creation failed");
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = output.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(data->outputs[i].Get(), nullptr, &uav, descriptorAt(7 + i));
        }

        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC constants {};
        constants.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; constants.Width = ConstantBytes;
        constants.Height = 1; constants.DepthOrArraySize = 1; constants.MipLevels = 1;
        constants.SampleDesc.Count = 1; constants.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &constants,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&data->constants)), "guide constants allocation failed");
        void* mapped = nullptr; const D3D12_RANGE noRead { 0, 0 };
        Check(data->constants->Map(0, &noRead, &mapped), "guide constants mapping failed");
        std::memset(mapped, 0, ConstantBytes);
        std::memcpy(mapped, shared.data(), sizeof(shared));
        std::memcpy(static_cast<std::byte*>(mapped) + SharedAlignedBytes, pass.data(), sizeof(pass));
        data->constants->Unmap(0, nullptr);
        return std::shared_ptr<Work>(new Work(std::move(data)));
    }
    catch (const std::exception& error)
    {
        try { LOG_WARN("[FSRRR private guides] preparation refused: {}", error.what()); } catch (...) {}
        return {};
    }
    catch (...) { return {}; }
}

bool Work::Record(ID3D12GraphicsCommandList* list) noexcept
{
    if (_impl->attempted.exchange(true)) return false;
    try
    {
        Require(list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && OnDevice(list, _impl->device.Get()),
                "private guide list/device unsupported");
        // Register BEFORE any commands. Registry owns Work, never Work->ticket: no owner cycle.
        const auto retained = FSRDSubmission::Retain(_impl->device.Get(), list, shared_from_this());
        Require(bool(retained), "private guide submission retention unavailable");
        list->SetPipelineState(_impl->pipeline.Get());
        list->SetComputeRootSignature(_impl->root.Get());
        ID3D12DescriptorHeap* heaps[] = { _impl->descriptors.Get() };
        list->SetDescriptorHeaps(1, heaps);
        const auto constants = _impl->constants->GetGPUVirtualAddress();
        list->SetComputeRootConstantBufferView(0, constants);
        list->SetComputeRootConstantBufferView(1, constants + SharedAlignedBytes);
        auto table = _impl->descriptors->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(2, table);
        table.ptr += UINT64(7) * _impl->stride;
        list->SetComputeRootDescriptorTable(3, table);
        // Authenticated CS returns for padded threads and writes all three outputs for
        // every in-bounds pixel; no partial-coverage dependency on old contents.
        list->Dispatch((_impl->width + 15) / 16, (_impl->height + 15) / 16, 1);
        D3D12_RESOURCE_BARRIER barriers[3] {};
        for (UINT i = 0; i < 3; ++i)
        {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = _impl->outputs[i].Get();
            barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[i].Transition.StateAfter = Readable;
        }
        list->ResourceBarrier(3, barriers);
        return true;
    }
    catch (const std::exception& error)
    {
        try { LOG_WARN("[FSRRR private guides] recording failed; submitted owners remain retained: {}", error.what()); } catch (...) {}
        return false;
    }
    catch (...) { return false; }
}
} // namespace FSRD::CyberpunkGuidePass

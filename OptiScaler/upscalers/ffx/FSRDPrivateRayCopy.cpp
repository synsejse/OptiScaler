#include "pch.h"
#include "FSRDPrivateRayCopy.h"
#include "resource_tracking/FSRDSubmission.h"
#include <atomic>
#include <cstdint>
#include <limits>
#include <utility>

namespace FSRD::PrivateRayCopy
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT MaxDimension = 8192;
constexpr UINT64 MaxBytes = 256ull * 1024 * 1024;
constexpr D3D12_RESOURCE_STATES Readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr std::array<DXGI_FORMAT, 2> Formats { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT };

struct Refused { const char* reason; };
void Require(bool condition, const char* reason) { if (!condition) throw Refused { reason }; }

ComPtr<IUnknown> Identity(IUnknown* object)
{
    ComPtr<IUnknown> result;
    Require(object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result))) && result,
            "object identity unavailable");
    return result;
}
bool OnDevice(ID3D12DeviceChild* object, ID3D12Device* expected)
{
    ComPtr<ID3D12Device> device;
    return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&device))) && device &&
           Identity(device.Get()).Get() == Identity(expected).Get();
}
void Charge(ID3D12Device* device, const D3D12_RESOURCE_DESC& description, UINT64& total)
{
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &description);
    Require(allocation.SizeInBytes && allocation.SizeInBytes != std::numeric_limits<UINT64>::max() &&
            allocation.SizeInBytes <= MaxBytes - total, "ray-copy retained allocation budget exceeded");
    total += allocation.SizeInBytes;
}

std::array<D3D12_RESOURCE_DESC, 2> OutputDescriptions(UINT width, UINT height)
{
    std::array<D3D12_RESOURCE_DESC, 2> descriptions {};
    for (UINT i = 0; i < descriptions.size(); ++i)
    {
        auto& output = descriptions[i];
        output.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        output.Width = width; output.Height = height;
        output.DepthOrArraySize = 1; output.MipLevels = 1;
        output.Format = Formats[i]; output.SampleDesc.Count = 1;
        output.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        output.Flags = D3D12_RESOURCE_FLAG_NONE;
    }
    return descriptions;
}

// Submission owns this lease, not Work or any submission ticket. No backreferences.
struct Lease
{
    ComPtr<ID3D12Device> device;
    Textures sources;
    Textures outputs;
    std::shared_ptr<Targets> targets;
};
} // namespace

struct Targets::Impl
{
    ComPtr<ID3D12Device> device;
    Textures outputs;
    UINT width = 0, height = 0;
    std::atomic<bool> claimed = false;
};
Targets::Targets(std::unique_ptr<Impl> implementation) : _impl(std::move(implementation)) {}
Targets::~Targets() = default;
const Textures& Targets::Outputs() const noexcept { return _impl->outputs; }

std::shared_ptr<Targets> AllocateTargets(ID3D12Device* device, UINT width, UINT height,
                                       const char** error) noexcept
{
    if (error) *error = "";
    try
    {
        Require(device && width && height && width <= MaxDimension && height <= MaxDimension,
                "invalid ray-copy device or extent");
        Identity(device);
        const auto descriptions = OutputDescriptions(width, height);
        UINT64 totalBytes = 0;
        for (const auto& output : descriptions) Charge(device, output, totalBytes);
        auto data = std::make_unique<Targets::Impl>();
        data->device = device; data->width = width; data->height = height;
        ScopedSkipHeapCapture skipHeapCapture {};
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
        for (UINT i = 0; i < data->outputs.size(); ++i)
            Require(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &descriptions[i],
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&data->outputs[i]))) && data->outputs[i],
                    "private ray-copy destination creation failed");
        return std::shared_ptr<Targets>(new Targets(std::move(data)));
    }
    catch (const Refused& refused) { if (error) *error = refused.reason; }
    catch (...) { if (error) *error = "ray-copy target allocation failed"; }
    return {};
}

struct Work::Impl
{
    std::shared_ptr<Lease> lease;
    UINT width = 0, height = 0;
    const char* error = "";
    std::atomic<bool> attempted = false;
    bool recorded = false;
};

Work::Work(std::unique_ptr<Impl> implementation) : _impl(std::move(implementation)) {}
Work::~Work() = default;
bool Work::Recorded() const noexcept { return _impl->recorded; }
std::string_view Work::Error() const noexcept { return _impl->error; }
const Textures& Work::Outputs() const noexcept { return _impl->lease->outputs; }

std::shared_ptr<Work> Prepare(ID3D12Device* device, UINT width, UINT height,
                              const Textures& sources, const char** error) noexcept
{
    return Work::PrepareImpl(device, width, height, sources, {}, false, error);
}

std::shared_ptr<Work> PrepareInto(ID3D12Device* device, UINT width, UINT height,
    const Textures& sources, const std::shared_ptr<Targets>& targets, const char** error) noexcept
{
    return Work::PrepareImpl(device, width, height, sources, targets, true, error);
}

std::shared_ptr<Work> Work::PrepareImpl(ID3D12Device* device, UINT width, UINT height,
    const Textures& sources, const std::shared_ptr<Targets>& supplied, bool preallocated,
    const char** error) noexcept
{
    if (error) *error = "";
    try
    {
        auto targets = supplied;
        if (preallocated)
        {
            Require(bool(targets), "ray-copy target set absent");
            Require(!targets->_impl->claimed.exchange(true), "ray-copy target set already claimed");
            Require(device && width == targets->_impl->width && height == targets->_impl->height &&
                    Identity(device).Get() == Identity(targets->_impl->device.Get()).Get(),
                    "ray-copy target device or extent differs");
        }
        Require(device && width && height && width <= MaxDimension && height <= MaxDimension,
                "invalid ray-copy device or extent");
        auto data = std::make_unique<Work::Impl>();
        data->lease = std::make_shared<Lease>();
        auto& lease = *data->lease;
        lease.device = device; lease.sources = sources;
        data->width = width; data->height = height;
        UINT64 totalBytes = 0;
        std::array<ComPtr<IUnknown>, 2> identities;
        const auto outputDescriptions = OutputDescriptions(width, height);
        for (UINT i = 0; i < sources.size(); ++i)
        {
            Require(sources[i] && OnDevice(sources[i].Get(), device), "ray-copy source missing or on another device");
            identities[i] = Identity(sources[i].Get());
            const auto source = sources[i]->GetDesc();
            Require(source.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    source.Width >= width && source.Height >= height && source.DepthOrArraySize == 1 &&
                    source.MipLevels == 1 && source.SampleDesc.Count == 1 && source.SampleDesc.Quality == 0 &&
                    source.Format == Formats[i], "unsupported native ray-copy source or active extent");
            D3D12_FEATURE_DATA_FORMAT_INFO formatInfo {};
            formatInfo.Format = source.Format;
            Require(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo))) &&
                    formatInfo.PlaneCount == 1, "ray-copy source must have exactly one native plane");
            Charge(device, source, totalBytes);
            const auto& output = outputDescriptions[i];
            Charge(device, output, totalBytes);
        }
        Require(identities[0].Get() != identities[1].Get(), "ray-copy source identities alias");
        // Allocate only after the entire source + destination budget/admission passes.
        if (!preallocated)
        {
            const char* allocationError = "";
            targets = AllocateTargets(device, width, height, &allocationError);
            Require(bool(targets), allocationError);
            Require(!targets->_impl->claimed.exchange(true), "ray-copy target set already claimed");
        }
        lease.targets = targets;
        lease.outputs = targets->_impl->outputs;
        for (const auto& output : lease.outputs)
            for (const auto& sourceIdentity : identities)
                Require(Identity(output.Get()).Get() != sourceIdentity.Get(), "ray-copy source aliases private target");
        return std::shared_ptr<Work>(new Work(std::move(data)));
    }
    catch (const Refused& refused) { if (error) *error = refused.reason; }
    catch (...) { if (error) *error = "ray-copy preparation failed"; }
    return {};
}

bool Work::Record(ID3D12GraphicsCommandList* originalDirectList) noexcept
{
    if (_impl->attempted.exchange(true)) return false;
    try
    {
        auto& lease = *_impl->lease;
        Require(originalDirectList && originalDirectList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                OnDevice(originalDirectList, lease.device.Get()), "ray-copy requires the original same-device direct list");
        const auto retained = FSRDSubmission::Retain(lease.device.Get(), originalDirectList, _impl->lease);
        Require(bool(retained), "ray-copy submission retention unavailable");

        // Native source COPY_SOURCE admission is exclusively the caller's contract.
        // No source StateBefore, binding changes, shader arithmetic or engine calls.
        const D3D12_BOX active { 0, 0, 0, _impl->width, _impl->height, 1 };
        std::array<D3D12_RESOURCE_BARRIER, 2> readable {};
        for (UINT i = 0; i < lease.outputs.size(); ++i)
        {
            D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
            source.pResource = lease.sources[i].Get(); destination.pResource = lease.outputs[i].Get();
            source.Type = destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = destination.SubresourceIndex = 0;
            originalDirectList->CopyTextureRegion(&destination, 0, 0, 0, &source, &active);
            auto& barrier = readable[i];
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = lease.outputs[i].Get();
            barrier.Transition.Subresource = 0;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = Readable;
        }
        originalDirectList->ResourceBarrier(UINT(readable.size()), readable.data());
        _impl->recorded = true;
        return true;
    }
    catch (const Refused& refused) { _impl->error = refused.reason; }
    catch (...) { _impl->error = "ray-copy recording failed; private outputs are not usable"; }
    return false;
}
} // namespace FSRD::PrivateRayCopy

#include "pch.h"
#include "FSRDPrivateDenoise.h"
#include "proxies/FfxApi_Proxy.h"
#include "resource_tracking/FSRDSubmission.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace FSRD::PrivateDenoise
{
namespace
{
using Microsoft::WRL::ComPtr;
using Converter = FSRDPreprocessor_Dx12;
constexpr uint64_t MaxBytes = 256ull * 1024 * 1024;
constexpr uint32_t MaxDimension = 8192;

struct Refused { const char* reason; };
void Require(bool condition, const char* reason) { if (!condition) throw Refused { reason }; }

// The Windows project uses /fp:fast: classification must not assume finite inputs.
bool Finite(float value) { return (std::bit_cast<uint32_t>(value) & 0x7f800000u) != 0x7f800000u; }
bool Finite(const DirectX::XMFLOAT4& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z) && Finite(value.w);
}
bool Finite(const DirectX::XMFLOAT4X4& value)
{
    for (const auto& row : value.m)
        for (const float element : row)
            if (!Finite(element)) return false;
    return true;
}
bool Finite(const FfxApiFloatCoords3D& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}
bool Unit(const FfxApiFloatCoords3D& value)
{
    const float length2 = value.x * value.x + value.y * value.y + value.z * value.z;
    return Finite(value) && Finite(length2) && std::abs(length2 - 1.0f) <= 0.01f;
}

void ValidateParameters(const Parameters& parameters)
{
    const auto& c = parameters.conversion;
    const auto& d = parameters.dispatch;
    const auto maximum = parameters.maxRenderSize;
    Require(d.renderSize.width && d.renderSize.height && maximum.width >= d.renderSize.width &&
            maximum.height >= d.renderSize.height && maximum.width <= MaxDimension && maximum.height <= MaxDimension,
            "invalid active/context extent");
    // Nine converted/denoiser textures use 52 bytes/texel, composition uses another 8.
    // Conservative capacity accounting also charges composed output at maximum size.
    Require(uint64_t(maximum.width) * maximum.height * 60 <= MaxBytes, "private texture budget exceeded");
    Require(Finite(c.RenderSize) && c.RenderSize.x == float(d.renderSize.width) &&
            c.RenderSize.y == float(d.renderSize.height) && c.RenderSize.z > 0 && c.RenderSize.w > 0 &&
            std::abs(c.RenderSize.z * c.RenderSize.x - 1.0f) <= 1e-6f &&
            std::abs(c.RenderSize.w * c.RenderSize.y - 1.0f) <= 1e-6f, "conversion extent/reciprocal mismatch");
    constexpr uint32_t allowedConversion = uint32_t(Converter::ConvFlags::NonGammaAlbedo) |
        uint32_t(Converter::ConvFlags::IsDepthLinear) | uint32_t(Converter::ConvFlags::IsRoughnessPacked) |
        uint32_t(Converter::ConvFlags::IsRightHanded) | uint32_t(Converter::ConvFlags::CyberpunkDepthMotion) |
        uint32_t(Converter::ConvFlags::ResetMotionHistory);
    Require((c.Flags & ~allowedConversion) == 0 && (c.Flags & uint32_t(Converter::ConvFlags::NonGammaAlbedo)),
            "only normal linear-albedo conversion is supported");
    Require(!(c.Flags & uint32_t(Converter::ConvFlags::CyberpunkDepthMotion)) ||
            !(c.Flags & uint32_t(Converter::ConvFlags::IsDepthLinear)), "hardware-depth motion requires hardware depth");
    Require((d.flags & ~(FFX_DENOISER_DISPATCH_RESET | FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO)) == 0 &&
            (d.flags & FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO), "dispatch/albedo encoding mismatch");
    Require(!d.header.pNext && !d.commandList &&
            (!d.header.type || d.header.type == FFX_API_DISPATCH_DESC_TYPE_DENOISER),
            "dispatch must be a scalar template without borrowed chains/list");
    Require(Finite(c.InvViewMatrix) && Finite(c.InvProjMatrix) && Finite(c.PrevViewMatrix) &&
            Finite(c.PreviousDepthProjection), "nonfinite conversion camera");
    Require(Finite(c.NearPlane) && Finite(c.FarPlane) && c.NearPlane > 0 && c.FarPlane > c.NearPlane &&
            d.cameraNear == c.NearPlane && d.cameraFar == c.FarPlane, "invalid/mismatched depth range");
    Require(Unit(d.cameraRight) && Unit(d.cameraUp) && Unit(d.cameraForward) && Finite(d.cameraPositionDelta) &&
            Finite(d.motionVectorScale) && Finite(d.jitterOffsets.x) && Finite(d.jitterOffsets.y) &&
            Finite(d.cameraAspectRatio) && d.cameraAspectRatio > 0 &&
            Finite(d.cameraFovAngleVertical) && d.cameraFovAngleVertical > 0 && d.cameraFovAngleVertical < 3.141593f &&
            Finite(d.deltaTime) && d.deltaTime > 0, "invalid explicit dispatch camera/time");
    const auto& s = parameters.settings;
    Require(Finite(s.crossBilateralNormalStrength) && Finite(s.stabilityBias) &&
            Finite(s.maxRadiance) && s.maxRadiance > 0 && Finite(s.radianceClipStdK) &&
            Finite(s.gaussianKernelRelaxation) && Finite(s.disocclusionThreshold),
            "nonfinite/missing explicit provider settings");
}

ComPtr<IUnknown> Identity(IUnknown* object)
{
    ComPtr<IUnknown> result;
    Require(object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result))), "object identity unavailable");
    return result;
}
bool OnDevice(ID3D12DeviceChild* object, ID3D12Device* expected)
{
    ComPtr<ID3D12Device> device;
    return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&device))) &&
           Identity(device.Get()).Get() == Identity(expected).Get();
}

// Narrow resource-only views for this diagnostic. Typed inputs avoid guessing a game's
// SRV interpretation; only the established hardware-depth families admit typeless views.
bool InputFormat(UINT role, DXGI_FORMAT format, bool hardwareDepth)
{
    if (role == 1)
    {
        if (!hardwareDepth) return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
        switch (format)
        {
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return true;
        default: return false;
        }
    }
    if (role == 4 || role == 5)
        return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT ||
               (role == 4 && (format == DXGI_FORMAT_R8_UNORM || format == DXGI_FORMAT_R16_UNORM));
    if (role == 2)
        return format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT ||
               format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (role == 3)
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (role == 0)
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
               format == DXGI_FORMAT_R11G11B10_FLOAT;
    return (role == 6 || role == 7) &&
           (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
            format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT);
}

std::string SelectProvider(ID3D12Device* device, uint64_t requested)
{
    uint64_t count = 0;
    ffxQueryDescGetVersions query {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_EFFECT_ID_DENOISER;
    query.device = device;
    query.outputCount = &count;
    Require(FfxApiProxy::D3D12_Query(nullptr, &query.header) == FFX_API_RETURN_OK && count && count <= 64,
            "RR provider enumeration unavailable/out of bounds");
    const auto capacity = count;
    std::vector<uint64_t> ids(size_t(count), 0);
    std::vector<const char*> names(size_t(count), nullptr);
    query.versionIds = ids.data(); query.versionNames = names.data();
    Require(FfxApiProxy::D3D12_Query(nullptr, &query.header) == FFX_API_RETURN_OK && count == capacity,
            "RR provider enumeration changed/failed");
    const auto found = std::find(ids.begin(), ids.end(), requested);
    Require(found != ids.end() && std::count(ids.begin(), ids.end(), requested) == 1,
            "explicit RR provider ID absent/ambiguous");
    const char* name = names[size_t(found - ids.begin())];
    return name ? std::string(name) : std::string();
}

// A ticket owns this lease, NEVER Work or its converter. Converter slots themselves own
// tickets, so retaining Work would form Work -> converter -> ticket -> Work forever.
struct Lease
{
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>, 8> inputs;
    Textures textures;
    DenoiserCore<FfxApiProxy> denoiser; // Destroyed first, while resources/device still exist.
};
} // namespace

struct Work::Impl
{
    std::shared_ptr<Lease> lease;
    std::unique_ptr<Converter> converter;
    Parameters parameters;
    std::string providerName;
    const char* error = "";
    std::atomic<bool> attempted = false;
    bool recorded = false;
};

Work::Work(std::unique_ptr<Impl> implementation) : _impl(std::move(implementation)) {}
Work::~Work() = default;
bool Work::Recorded() const noexcept { return _impl->recorded; }
std::string_view Work::Error() const noexcept { return _impl->error; }
const Textures& Work::Outputs() const noexcept { return _impl->lease->textures; }
const Parameters& Work::EffectiveParameters() const noexcept { return _impl->parameters; }
std::string_view Work::ProviderName() const noexcept { return _impl->providerName; }

std::shared_ptr<Work> Prepare(ID3D12Device* device, const Parameters& parameters, const char** error) noexcept
{
    if (error) *error = "";
    try
    {
        Require(device, "device absent");
        ValidateParameters(parameters);
        auto data = std::make_unique<Work::Impl>();
        data->parameters = parameters;
        data->parameters.conversion.Flags |= uint32_t(Converter::ConvFlags::ResetMotionHistory);
        data->parameters.dispatch.flags |= FFX_DENOISER_DISPATCH_RESET;
        // Never retain caller-provided FFX resources/chains, even as misleading metadata.
        auto& d = data->parameters.dispatch;
        d.header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER };
        d.linearDepth = {}; d.motionVectors = {}; d.normals = {}; d.specularAlbedo = {}; d.diffuseAlbedo = {};
        data->lease = std::make_shared<Lease>();
        auto& lease = *data->lease;
        lease.device = device;
        const bool packed = parameters.conversion.Flags & uint32_t(Converter::ConvFlags::IsRoughnessPacked);
        const bool hardwareDepth = !(parameters.conversion.Flags & uint32_t(Converter::ConvFlags::IsDepthLinear));
        uint64_t inputBytes = 0;
        std::vector<ComPtr<IUnknown>> identities;
        for (UINT i = 0; i < lease.inputs.size(); ++i)
        {
            if (i == 4 && packed)
            {
                data->parameters.conversion.Resources.AsArray[i] = nullptr;
                continue;
            }
            auto* source = parameters.conversion.Resources.AsArray[i];
            Require(source, "required native input absent");
            lease.inputs[i] = source; // Valid caller borrow; own before all following checks.
            Require(OnDevice(source, device), "input device differs");
            const auto desc = source->GetDesc();
            Require(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
                    desc.MipLevels >= 1 && desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
                    desc.Width >= d.renderSize.width && desc.Height >= d.renderSize.height &&
                    !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) && InputFormat(i, desc.Format, hardwareDepth),
                    "unsupported native input view/extent");
            const auto identity = Identity(source);
            if (std::none_of(identities.begin(), identities.end(), [&](const auto& item) { return item.Get() == identity.Get(); }))
            {
                const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
                Require(allocation.SizeInBytes && allocation.SizeInBytes != UINT64_MAX &&
                        allocation.SizeInBytes <= MaxBytes - inputBytes, "owned input budget exceeded");
                inputBytes += allocation.SizeInBytes;
                identities.push_back(identity);
            }
        }

        ScopedSkipSpoofingGlobal skipSpoofing {};
        data->providerName = SelectProvider(device, parameters.providerId);
        ffxOverrideVersion version { .header = { .type = FFX_API_DESC_TYPE_OVERRIDE_VERSION },
                                     .versionId = parameters.providerId };
        ffxCreateBackendDX12Desc backend { .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
                                                       .pNext = &version.header }, .device = device };
        ffxCreateContextDescDenoiser create { .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER,
                                                          .pNext = &backend.header },
            .version = FFX_DENOISER_VERSION, .maxRenderSize = parameters.maxRenderSize,
            .mode = FFX_DENOISER_MODE_1_SIGNAL, .flags = 0 };
        ScopedSkipHeapCapture skipHeapCapture {};
        Require(lease.denoiser.Create(create) == FFX_API_RETURN_OK, "private RR context creation failed");
        Require(lease.denoiser.QueryDefaults().code == FFX_API_RETURN_OK, "private RR defaults query failed");
        Require(lease.denoiser.Configure(parameters.settings).code == FFX_API_RETURN_OK, "private RR configuration failed");
        data->parameters.settings = lease.denoiser.Settings();
        data->converter = std::make_unique<Converter>("FSRD Private Reset", device);
        Require(data->converter->IsInit() && data->converter->SetMaxRenderSize(parameters.maxRenderSize.width,
                    parameters.maxRenderSize.height), "private converter allocation failed");
        ffxDispatchDescDenoiserInput1Signal signal {};
        ffxDispatchDescDenoiser dispatch {};
        data->converter->GetSignal(signal, dispatch);
        auto& out = lease.textures;
        out.radiance = static_cast<ID3D12Resource*>(signal.radiance.input.resource);
        out.denoised = static_cast<ID3D12Resource*>(signal.radiance.output.resource);
        out.fusedAlbedo = static_cast<ID3D12Resource*>(signal.fusedAlbedo.resource);
        out.depth = static_cast<ID3D12Resource*>(dispatch.linearDepth.resource);
        out.motion = static_cast<ID3D12Resource*>(dispatch.motionVectors.resource);
        out.normals = static_cast<ID3D12Resource*>(dispatch.normals.resource);
        out.diffuseAlbedo = static_cast<ID3D12Resource*>(dispatch.diffuseAlbedo.resource);
        out.specularAlbedo = static_cast<ID3D12Resource*>(dispatch.specularAlbedo.resource);
        out.preservedLighting = data->converter->GetPreservedLighting();
        Require(out.radiance && out.denoised && out.fusedAlbedo && out.depth && out.motion && out.normals &&
                out.diffuseAlbedo && out.specularAlbedo && out.preservedLighting, "private converter resources absent");
        return std::shared_ptr<Work>(new Work(std::move(data)));
    }
    catch (const Refused& refusal) { if (error) *error = refusal.reason; }
    catch (...) { if (error) *error = "private RR preparation failed"; }
    return {};
}

bool Work::Record(ID3D12GraphicsCommandList* list) noexcept
{
    if (_impl->attempted.exchange(true)) return false;
    auto& data = *_impl;
    bool writable = false;
    try
    {
        Require(list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && OnDevice(list, data.lease->device.Get()),
                "private RR requires a direct list on the prepared device");
        // Acyclic lease is durable even if conversion/AMD/composition records then fails.
        const auto retained = FSRDSubmission::Retain(data.lease->device.Get(), list, data.lease);
        Require(bool(retained), "private RR submission retention unavailable");
        ScopedSkipHeapCapture skipHeapCapture {};
        Require(data.converter->DispatchConversion(list, data.parameters.conversion), "private conversion failed");
        auto dispatch = data.parameters.dispatch;
        ffxDispatchDescDenoiserInput1Signal signal {};
        data.converter->GetSignal(signal, dispatch);
        dispatch.commandList = list;
        data.converter->SetDenoiserOutputsWritable(list, true);
        writable = true;
        const auto result = data.lease->denoiser.Dispatch(dispatch);
        data.converter->SetDenoiserOutputsWritable(list, false);
        writable = false;
        Require(result == FFX_API_RETURN_OK, "private RR dispatch failed");
        const Converter::CompositionDesc composition { .DstTexSize = data.parameters.conversion.RenderSize, .Flags = 0 };
        Require(data.converter->DispatchComposition(list, composition, false), "private composition failed");
        // Composition's own retained Storage already owns this resource before its commands.
        data.lease->textures.composed = data.converter->GetCompositionOutput();
        Require(data.lease->textures.composed != nullptr, "private composition output absent");
        data.recorded = true;
        return true;
    }
    catch (const Refused& refusal) { data.error = refusal.reason; }
    catch (...) { data.error = "private RR recording failed"; }
    if (writable)
    {
        // Provider exceptions must not strand our private output in UAV state. The source
        // engine bindings still belong to the caller, including on this failure path.
        try { data.converter->SetDenoiserOutputsWritable(list, false); }
        catch (...) {} // Keep the lease retained; never claim a usable output after failure.
    }
    return false;
}
} // namespace FSRD::PrivateDenoise

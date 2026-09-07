#include "pch.h"
#include "FSRDCyberpunkFogRgbWrite.h"
#include "resource_tracking/FSRDSubmission.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <utility>

namespace FSRD::CyberpunkFogRgbWrite
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT MaxDimension = 8192;
constexpr UINT64 MaxRetainedBytes = 256ull * 1024 * 1024;
constexpr char Shader[] = R"(
Texture2D<float4> Input : register(t0);
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    VertexOutput output;
    output.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = p;
    return output;
}
float4 PSMain(sample float2 uv : TEXCOORD0) : SV_Target0
{
    // SV_Position may describe a coarse VRS region even for sample-frequency
    // execution. Use the actual sample-interpolated coordinate instead. With
    // W=1 at every vertex and an exact full viewport, this maps each fine
    // sample to its own texel; the admitted source/target extents are equal.
    uint width, height;
    Input.GetDimensions(width, height);
    return float4(Input.Load(int3(uint2(uv * float2(width, height)), 0)).rgb, 0);
}
)";

struct Refused { const char* reason; };
void Require(bool value, const char* reason) { if (!value) throw Refused { reason }; }
ComPtr<IUnknown> Identity(IUnknown* object)
{
    ComPtr<IUnknown> result;
    Require(object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&result))) && result,
            "RGB-write object identity unavailable");
    return result;
}
bool OnDevice(ID3D12DeviceChild* object, ID3D12Device* expected)
{
    ComPtr<ID3D12Device> actual;
    return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&actual))) && actual &&
        Identity(actual.Get()).Get() == Identity(expected).Get();
}
bool Exact(float actual, float expected)
{
    // Bit comparisons stay fail-closed for NaN/Inf under project /fp:fast.
    return std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(expected);
}
void ValidateTexture(ID3D12Device* device, ID3D12Resource* resource, UINT width, UINT height,
                     bool target, UINT64& retained)
{
    Require(OnDevice(resource, device), "RGB-write texture device mismatch");
    const auto d = resource->GetDesc();
    Require(d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width && d.Height == height &&
            d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0 &&
            d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && d.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
            !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), "unsupported RGB-write texture or extent");
    Require(target ? bool(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) :
                     !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE), "RGB-write texture usage unavailable");
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &d);
    Require(allocation.SizeInBytes && allocation.SizeInBytes != std::numeric_limits<UINT64>::max() &&
            allocation.SizeInBytes <= MaxRetainedBytes - retained, "RGB-write retained texture budget exceeded");
    retained += allocation.SizeInBytes;
}
ComPtr<ID3DBlob> Compile(const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> shader, errors;
    const auto result = D3DCompile(Shader, sizeof(Shader) - 1, "FSRDCyberpunkFogRgbWrite", nullptr, nullptr,
                                   entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                   0, &shader, &errors);
    if (!SUCCEEDED(result) || !shader || !shader->GetBufferPointer() || !shader->GetBufferSize())
    {
        try
        {
            constexpr size_t MaxDiagnosticBytes = 2048;
            const auto size = errors ? errors->GetBufferSize() : 0;
            const auto* data = errors ? static_cast<const char*>(errors->GetBufferPointer()) : nullptr;
            const std::string_view diagnostic = data ? std::string_view(data, size < MaxDiagnosticBytes ? size : MaxDiagnosticBytes)
                                                     : std::string_view {};
            LOG_WARN("[FSRRR RGB identity] {} {} compilation HRESULT={:08x}: {}", entry, profile, uint32_t(result), diagnostic);
        }
        catch (...) {} // Logging must not replace the static bounded error result.
        throw Refused { "RGB-write shader compilation failed" };
    }
    return shader;
}

// Submission retains only this leaf owner. It owns neither Work nor a ticket,
// parent capture plan or callback; partial command recording cannot create a cycle.
struct Lease
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> source, target;
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv {};
    D3D12_GPU_DESCRIPTOR_HANDLE srv {};
};
} // namespace

struct Work::Impl
{
    std::shared_ptr<Lease> lease;
    std::atomic<bool> attempted = false, recorded = false;
    std::atomic<const char*> error { "" };
};
Work::Work(std::unique_ptr<Impl> implementation) : _impl(std::move(implementation)) {}
Work::~Work() = default;
bool Work::Recorded() const noexcept { return _impl->recorded.load(std::memory_order_acquire); }
std::string_view Work::Error() const noexcept { return _impl->error.load(std::memory_order_acquire); }

std::shared_ptr<Work> Prepare(ID3D12Device* device, UINT width, UINT height,
    const ComPtr<ID3D12Resource>& source, const ComPtr<ID3D12Resource>& target,
    const D3D12_RENDER_TARGET_VIEW_DESC& originalView,
    const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, const char** error) noexcept
{
    if (error) *error = "";
    try
    {
        Require(device && source && target && width && height && width <= MaxDimension && height <= MaxDimension,
                "invalid RGB-write input or extent");
        Require(Identity(source.Get()).Get() != Identity(target.Get()).Get(), "RGB-write source aliases native target");
        Require(Exact(viewport.TopLeftX, 0) && Exact(viewport.TopLeftY, 0) &&
                Exact(viewport.Width, float(width)) && Exact(viewport.Height, float(height)) &&
                Exact(viewport.MinDepth, 0) && Exact(viewport.MaxDepth, 1) &&
                scissor.left == 0 && scissor.top == 0 && scissor.right == LONG(width) && scissor.bottom == LONG(height),
                "RGB-write requires exact full viewport and scissor");
        Require(originalView.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                originalView.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D &&
                originalView.Texture2D.MipSlice == 0 && originalView.Texture2D.PlaneSlice == 0,
                "RGB-write original RTV is not exact RGBA16F mip0/plane0");
        UINT64 retained = 0;
        ValidateTexture(device, source.Get(), width, height, false, retained);
        ValidateTexture(device, target.Get(), width, height, true, retained);
        auto data = std::make_unique<Work::Impl>();
        data->lease = std::make_shared<Lease>();
        auto& lease = *data->lease;
        lease.device = device; lease.source = source; lease.target = target;

        const auto vs = Compile("VSMain", "vs_5_0");
        const auto ps = Compile("PSMain", "ps_5_0");
        ComPtr<ID3D11ShaderReflection> reflection;
        Require(SUCCEEDED(D3DReflect(ps->GetBufferPointer(), ps->GetBufferSize(), IID_PPV_ARGS(&reflection))) &&
                reflection && reflection->IsSampleFrequencyShader(), "RGB-write sample-frequency proof unavailable");

        D3D12_DESCRIPTOR_RANGE range {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameter.DescriptorTable.NumDescriptorRanges = 1; parameter.DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC root {};
        root.NumParameters = 1; root.pParameters = &parameter;
        ComPtr<ID3DBlob> serialized, errors;
        Require(SUCCEEDED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) &&
                serialized && serialized->GetBufferPointer() && serialized->GetBufferSize(), "RGB-write root serialization failed");
        Require(SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                     IID_PPV_ARGS(&lease.root))) && lease.root, "RGB-write root creation failed");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = lease.root.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        for (auto& b : pso.BlendState.RenderTarget)
        {
            b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ZERO; b.BlendOp = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD; b.LogicOp = D3D12_LOGIC_OP_NOOP;
        }
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        pso.SampleMask = std::numeric_limits<UINT>::max();
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        auto& front = pso.DepthStencilState.FrontFace;
        front.StencilFailOp = front.StencilDepthFailOp = front.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        front.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.BackFace = front;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pso.SampleDesc.Count = 1;
        Require(SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&lease.pipeline))) && lease.pipeline,
                "RGB-write pipeline creation failed");

        ScopedSkipHeapCapture skipHeapCapture {};
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Require(SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&lease.srvHeap))) && lease.srvHeap,
                "RGB-write SRV heap creation failed");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Require(SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&lease.rtvHeap))) && lease.rtvHeap,
                "RGB-write RTV heap creation failed");
        const auto cpuSrv = lease.srvHeap->GetCPUDescriptorHandleForHeapStart();
        lease.srv = lease.srvHeap->GetGPUDescriptorHandleForHeapStart();
        lease.rtv = lease.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        Require(cpuSrv.ptr && lease.srv.ptr && lease.rtv.ptr, "RGB-write private descriptor handles absent");
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(lease.source.Get(), &srv, cpuSrv);
        device->CreateRenderTargetView(lease.target.Get(), &originalView, lease.rtv);
        return std::shared_ptr<Work>(new Work(std::move(data)));
    }
    catch (const Refused& refused) { if (error) *error = refused.reason; }
    catch (...) { if (error) *error = "RGB-write preparation failed"; }
    return {};
}

bool Work::Record(ID3D12GraphicsCommandList* list) noexcept
{
    if (_impl->attempted.exchange(true)) return false;
    try
    {
        const auto& lease = *_impl->lease;
        Require(list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && OnDevice(list, lease.device.Get()),
                "RGB-write requires the admitted device DIRECT list");
        const auto retained = FSRDSubmission::Retain(lease.device.Get(), list, _impl->lease);
        Require(bool(retained), "RGB-write lifetime retention unavailable");
        list->SetGraphicsRootSignature(lease.root.Get());
        ID3D12DescriptorHeap* heaps[] = { lease.srvHeap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(0, lease.srv);
        list->SetPipelineState(lease.pipeline.Get());
        list->OMSetRenderTargets(1, &lease.rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        _impl->recorded.store(true, std::memory_order_release);
        return true;
    }
    catch (const Refused& refused) { _impl->error.store(refused.reason, std::memory_order_release); }
    catch (...) { _impl->error.store("RGB-write recording failed; caller must restore graphics state", std::memory_order_release); }
    return false;
}
} // namespace FSRD::CyberpunkFogRgbWrite

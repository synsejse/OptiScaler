#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string_view>

namespace FSRD::CyberpunkFogRgbWrite
{
class Work
{
  public:
    ~Work();
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    // One attempt, including invalid-list/retention failures; never retry the
    // same Work. Retains an acyclic resource/descriptor/pipeline lease BEFORE
    // its first command. Recorded means recorded, NOT submitted or completed.
    // False after partial recording still requires caller graphics restoration.
    bool Record(ID3D12GraphicsCommandList* list) noexcept;
    bool Recorded() const noexcept;
    std::string_view Error() const noexcept;

  private:
    struct Impl;
    explicit Work(std::unique_ptr<Impl> implementation);
    friend std::shared_ptr<Work> Prepare(ID3D12Device*, UINT, UINT,
        const Microsoft::WRL::ComPtr<ID3D12Resource>&,
        const Microsoft::WRL::ComPtr<ID3D12Resource>&,
        const D3D12_RENDER_TARGET_VIEW_DESC&, const D3D12_VIEWPORT&, const D3D12_RECT&, const char**) noexcept;
    std::unique_ptr<Impl> _impl;
};

// CPU-only preparation, inactive until explicitly recorded by an admitted host.
// Source is an OWNED PRIVATE RGBA16F snapshot/recomposition; target is the
// original-use native RGBA16F Fog target with the exact observed RTV description.
// Both must have exactly this extent, one mip/slice/sample, on the same device.
// A private frozen RTV is created from the retained target and validated view:
// no caller-owned mutable CPU descriptor is copied or later reread.
//
// Caller must independently authenticate current Fog scope/list/Reset/target
// lifetime, full viewport/scissor, TRIANGLELIST topology and source provenance.
// At Record, the SAME viewport/scissor remain installed, source is readable
// (PIXEL_SHADER_RESOURCE, possibly its combined-read superset), target is RT,
// and all producer dependencies are proved/enforced before native submission.
// No active predication/query/render-pass/bundle or unsupported coverage state.
// Allocation/format validation is NOT source-readiness or scene-write authority.
//
// This helper changes graphics root, descriptor heaps/tables, PSO and OM, then
// draws three vertices. It never changes IA, RS, VRS or resource states. Shader
// reflection must prove sample-frequency execution. Sample-interpolated UVs
// from the owned W=1 fullscreen triangle address the source, not SV_Position
// (which may name a coarse VRS region). A real identity roundtrip with the
// actual inherited rasterization/VRS state is still required before use.
// RGB-only write mask leaves original alpha UNWRITTEN. RGB still needs an actual
// identity roundtrip test, especially for unusual nonfinite/denormal values.
//
// Host MUST restore original roots/heaps/dynamic tables/PSO/frozen RTV on every
// post-mutation result before resuming the original Fog draw ONCE. This is a
// scene-write operation, NOT authorized by the private-compute-only wrapper.
std::shared_ptr<Work> Prepare(ID3D12Device* device, UINT width, UINT height,
    const Microsoft::WRL::ComPtr<ID3D12Resource>& source,
    const Microsoft::WRL::ComPtr<ID3D12Resource>& target,
    const D3D12_RENDER_TARGET_VIEW_DESC& originalView,
    const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor,
    const char** error = nullptr) noexcept;
} // namespace FSRD::CyberpunkFogRgbWrite

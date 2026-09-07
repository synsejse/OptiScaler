#pragma once

#include "FSRDCyberpunkGuideConstants.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace FSRD::CyberpunkGuidePass
{
struct SourceView
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor {};
};

class Work : public std::enable_shared_from_this<Work>
{
  public:
    ~Work();
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    // One attempted recording per Work. Retains this entire owner through the existing
    // submission observer BEFORE the first command. Failure does not make earlier GPU
    // references disposable. No engine-resource barriers or game-color writes are made.
    // Caller must restore engine roots/heaps/original PSO even when this returns false.
    bool Record(ID3D12GraphicsCommandList* list) noexcept;

    // u0 diffuse / u1 specular = native RGBA8_UNORM; u2 normal/roughness = RGBA16_FLOAT.
    // On successful Record all three are in NON_PIXEL|PIXEL_SHADER_RESOURCE state.
    const std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 3>& Outputs() const;

  private:
    struct Impl;
    explicit Work(std::unique_ptr<Impl> implementation);
    friend std::shared_ptr<Work> Prepare(ID3D12Device*, UINT, UINT,
        const std::array<SourceView, 4>&, const CyberpunkGuideConstants::PassConstants&,
        const CyberpunkGuideConstants::SharedConstants&, std::span<const std::byte>) noexcept;
    std::unique_ptr<Impl> _impl;
};

// CPU-only setup for an explicitly requested private-output experiment. This helper
// does not resolve engine resources, hook anything or read a shader from disk. Caller
// supplies runtime-extracted shader bytes; exact authenticated SHA/length are checked.
//
// sources order: t0 GBuffer0, t1 GBuffer1, t2 GBuffer2, t4 ALTERNATE STENCIL SRV.
// Caller authenticates each exact descriptor/view/generation, borrows/owns resources
// at a valid API/engine lifetime boundary, and prevents source-slot writes until the
// synchronous private descriptor copy finishes. A numeric handle alone is insufficient.
// Source heaps must be non-shader-visible CBV_SRV_UAV heaps from this device.
//
// Before Record, caller establishes current producer/alias/frame provenance and makes
// these exact source subresources readable via the engine's own transition protocol.
// Caller excludes predication, active queries, render passes and bundles using known
// complete command-list state. A skipped predicated dispatch is NOT a valid capture.
// Only inactive t3/t5/t6 is supported: b6 transparency and extra-specular flags MUST be
// zero. Null t6 is a structured buffer (stride28), not a Texture2D. This restriction is
// explicit, not silently disabling an authored branch that needs missing resources.
std::shared_ptr<Work> Prepare(ID3D12Device* device, UINT width, UINT height,
    const std::array<SourceView, 4>& sources, const CyberpunkGuideConstants::PassConstants& pass,
    const CyberpunkGuideConstants::SharedConstants& shared, std::span<const std::byte> shader) noexcept;
} // namespace FSRD::CyberpunkGuidePass

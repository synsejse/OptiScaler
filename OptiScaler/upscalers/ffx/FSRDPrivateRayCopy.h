#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <string_view>

namespace FSRD::PrivateRayCopy
{
// Index 0 is motion, index 1 is hit distance. The narrow supported native formats
// are RGBA16_FLOAT and R32_FLOAT respectively; unsupported formats are refused.
// Every channel/bit is copied, including motion Z/W, nonfinite values and signed zero.
using Textures = std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2>;

class Work;
// Fixed, factory-owned destinations. Allocation is NOT recorded production, readable
// state, completion or frame association. Only PrepareInto may claim a producer;
// consumers require the host's producer/consumer submission-order admission.
class Targets
{
  public:
    ~Targets();
    Targets(const Targets&) = delete;
    Targets& operator=(const Targets&) = delete;
    const Textures& Outputs() const noexcept;

  private:
    struct Impl;
    explicit Targets(std::unique_ptr<Impl> implementation);
    friend class Work;
    friend std::shared_ptr<Targets> AllocateTargets(ID3D12Device*, UINT, UINT, const char**) noexcept;
    std::unique_ptr<Impl> _impl;
};

// CPU-only allocation of exactly RGBA16_FLOAT/R32_FLOAT in COPY_DEST. No arbitrary
// caller textures can be adopted. Extent/device/resources never change afterward.
std::shared_ptr<Targets> AllocateTargets(ID3D12Device* device, UINT width, UINT height,
                                       const char** error = nullptr) noexcept;

class Work
{
  public:
    ~Work();
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    // One attempted recording, no retry. Success means commands were recorded,
    // NOT GPU completion or a frame association. Successful outputs are in combined
    // PIXEL/NON_PIXEL_SHADER_RESOURCE state for subsequent ordered same-list use.
    // False may leave partial private copies recorded; outputs must not be consumed.
    // Caller still performs its engine source-state restoration on either result.
    bool Record(ID3D12GraphicsCommandList* originalDirectList) noexcept;
    bool Recorded() const noexcept;
    std::string_view Error() const noexcept;
    const Textures& Outputs() const noexcept;

  private:
    struct Impl;
    explicit Work(std::unique_ptr<Impl> implementation);
    friend std::shared_ptr<Work> Prepare(ID3D12Device*, UINT, UINT, const Textures&, const char**) noexcept;
    friend std::shared_ptr<Work> PrepareInto(ID3D12Device*, UINT, UINT, const Textures&,
                                           const std::shared_ptr<Targets>&, const char**) noexcept;
    static std::shared_ptr<Work> PrepareImpl(ID3D12Device*, UINT, UINT, const Textures&,
                                            const std::shared_ptr<Targets>&, bool, const char**) noexcept;
    std::unique_ptr<Impl> _impl;
};

// CPU-only preparation takes owning references at a caller-validated current-use
// lifetime boundary. Sources must be typed, single-plane 2D, single mip/layer/sample;
// copies use mip0 and the explicit origin-zero active extent, not allocation padding.
// Source and private allocation accounting together is bounded to 256 MiB per Work.
// No format conversion, packing, shader dispatch, game writes or source barriers.
//
// Caller must prove before Record (not inferred from pointers/descriptions):
// - Both inputs belong to the intended current original ray invocation and are valid
//   borrowed resources, with their current contents ordered on that SAME direct list.
// - Each native source is in a read-only state containing COPY_SOURCE; a compatible
//   read-only superset is fine. Any required alias/UAV/transition barriers are already
//   recorded by the caller/engine. This helper never guesses a source StateBefore.
//   Placed/reserved backing heaps and tile mappings remain valid through completion;
//   owning a resource interface alone does not retain that backing allocation.
// - No active render pass/predication/query/bundle ambiguity. Any subsequent required
//   source-state restoration belongs to the caller, including after recording fails.
// - Calls/access to one Work are serialized; the recorded list is not submitted twice.
//
// Record only copies textures and transitions PRIVATE destinations. It does not touch
// PSO, state object, roots, heaps, viewports, render targets or native engine state.
// Before the first command an acyclic lease retains device, sources and destinations
// through the existing FSRDSubmission fence. It owns neither Work nor a ticket.
// Failed/unsubmitted work follows that registry's existing deliberate retention policy.
// Async consumers still need an actual completion fence; pointer identity is no proof.
// error receives a static message when supplied, with no dangling exception string.
std::shared_ptr<Work> Prepare(ID3D12Device* device, UINT width, UINT height,
                              const Textures& sources, const char** error = nullptr) noexcept;

// Same preparation/recording contract, with predeclared outputs. Exactly one atomic
// attempt claims a nonnull Targets, including failed preparation; never retry/rebind
// that set. Wrong device/extent, null targets and unsupported sources are refused.
// Producer/consumer recording may occur in either CPU order, but the host MUST prove
// actual successful production and GPU ordering before any consumer is submitted.
std::shared_ptr<Work> PrepareInto(ID3D12Device* device, UINT width, UINT height,
    const Textures& sources, const std::shared_ptr<Targets>& targets,
    const char** error = nullptr) noexcept;
} // namespace FSRD::PrivateRayCopy

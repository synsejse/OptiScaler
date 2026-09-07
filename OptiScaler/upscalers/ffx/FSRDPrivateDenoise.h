#pragma once

#include "FSRDDenoiserCore.h"
#include "shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string_view>

namespace FSRD::PrivateDenoise
{
struct Parameters
{
    FSRDPreprocessor_Dx12::ConversionDesc conversion {};
    // Scalar/camera template only: no extension chain or command list. Resource fields
    // are replaced with this Work's converted resources, never used as provider inputs.
    ffxDispatchDescDenoiser dispatch {};
    FfxApiDimensions2D maxRenderSize {};
    uint64_t providerId = 0;
    DenoiserSettings settings {}; // All six values are explicit; no "auto"/Config defaults.
};

struct Textures
{
    Microsoft::WRL::ComPtr<ID3D12Resource> radiance, denoised, fusedAlbedo, preservedLighting;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth, motion, normals, diffuseAlbedo, specularAlbedo;
    Microsoft::WRL::ComPtr<ID3D12Resource> composed;
};

class Work;
class Session;

struct SessionDesc
{
    FfxApiDimensions2D maxRenderSize {};
    uint64_t providerId = 0;
    DenoiserSettings settings {};
    uint64_t epoch = 0; // Caller-owned, nonzero software-session epoch, not a native view identity.
    static constexpr uint32_t MaxFrames = 18000;
    uint32_t frameLimit = 32; // Bounded visual pass; never wraps/restarts or periodically resets history.
};

struct FrameAdmission
{
    uint64_t epoch = 0;
    uintptr_t canonicalDirectQueue = 0; // Exact same-device DIRECT queue, authenticated by caller.
    bool sameViewAndCoordinateOrigin = false; // Caller attestation, never inferred from matrices.
};

class Session
{
  public:
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // HOST ATTESTATION, not independent GPU/submission proof or GPU completion:
    // this exact Work recorded successfully, its intended consumer/scene succeeded,
    // and that consumer's actual original Execute call RETURNED on this exact queue.
    // Caller must make this truthful before a subsequent frame can be prepared.
    // Same Execute-array order alone is NOT this acknowledgement. No lock is held
    // across Execute, and no engine/queue calls or waits occur in this method.
    bool AcknowledgeExecuted(const Work& frame, uintptr_t canonicalDirectQueue) noexcept;
    void Stop() noexcept; // Irreversible; does not release in-flight leases or pretend to undo history.
    bool Stopped() const noexcept;
    bool Complete() const noexcept; // Frame limit acknowledged, NOT GPU completion.
    uint32_t AcknowledgedFrames() const noexcept;

  private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> implementation);
    friend class Work;
    friend std::shared_ptr<Session> CreateSession(ID3D12Device*, const SessionDesc&, const char**) noexcept;
    friend std::shared_ptr<Work> PrepareFrame(const std::shared_ptr<Session>&, const Parameters&,
                                               const FrameAdmission&, const char**) noexcept;
    std::unique_ptr<Impl> _impl;
};

class Work
{
  public:
    ~Work();
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    // One attempted recording, no retries/fallback. True means commands were recorded,
    // NOT GPU completion. Every output is private and readable on successful recording.
    // False may leave private work recorded: caller MUST still restore engine roots,
    // descriptor heaps and PSO. This helper never changes an input's resource state.
    bool Record(ID3D12GraphicsCommandList* list) noexcept;
    bool Recorded() const noexcept;
    std::string_view Error() const noexcept;
    const Textures& Outputs() const noexcept;
    const Parameters& EffectiveParameters() const noexcept;
    std::string_view ProviderName() const noexcept;

  private:
    struct Impl;
    explicit Work(std::unique_ptr<Impl> implementation);
    friend class Session;
    friend std::shared_ptr<Work> Prepare(ID3D12Device*, const Parameters&, const char**) noexcept;
    friend std::shared_ptr<Work> PrepareFrame(const std::shared_ptr<Session>&, const Parameters&,
                                               const FrameAdmission&, const char**) noexcept;
    static std::unique_ptr<Impl> PrepareResources(ID3D12Device*, const Parameters&);
    static void PrepareConverter(Impl&, const char*);
    std::unique_ptr<Impl> _impl;
};

// CPU-only preparation, no hooks, module loading, game writes or GPU waits. Creates an
// independent RR context with the exact locally enumerated provider ID and settings.
// Forces only provider RESET and converter ResetMotionHistory; no borrowed late history.
// Does not apply exposure/encoding factors: Color must already be the caller-verified
// composed surface color at the intended boundary, in its existing linear working units.
// Uses the existing fused divide, quantization/residual, AMD dispatch and composition.
// The composed texture's alpha is the existing compositor's 1, NOT preserved scene alpha.
//
// Caller contract (not established by pointer/size validation):
// - All inputs are current, correctly encoded, origin-zero mip0 views of the same view
//   and intended frame; matrices/jitter/delta/settings are authenticated, not estimates.
// - Borrow resources at a valid lifetime boundary. Prepare takes owning references.
//   Earlier producer commands/descriptor storage must independently remain retained.
// - Each consumed input must be NON_PIXEL shader-readable at its GPU use, with
//   required ordering/alias barriers. For predeclared PRIVATE producer targets,
//   CPU Record may occur first only when an irrevocable pre-Execute obligation
//   enforces the successful producer's exact recording, terminal barriers and
//   same-queue dependency. Allocation alone establishes none of those facts.
// - The direct list has known state excluding predication, queries, render passes and
//   bundles. Restore all engine bindings after Record, including its failure paths.
// - Serialize preparation/recording with other provider management as required by the
//   existing FfxApiProxy. This primitive does not make that proxy globally thread-safe.
// - Do not submit the same recorded list twice. No persistent/live temporal mode here.
//
// GPU lifetime is an ACYCLIC lease retained before the first recorded command. It owns
// provider context + inputs + private outputs, but NOT Work/converter/submission ticket.
// Converter storage is retained separately. Existing submission fences retire owners;
// abandoned or failed submissions deliberately retain them under that existing policy.
// No generic keepAlive is accepted: an owner pointing back to a ticket would form a cycle.
// Diagnostic bounds: inputs <=256 MiB allocation accounting, converter textures <=256
// MiB nominal texels; these do not claim to bound opaque provider-internal allocations.
// error, if supplied, receives a static message (valid after this call and no allocation).
std::shared_ptr<Work> Prepare(ID3D12Device* device, const Parameters& parameters,
                              const char** error = nullptr) noexcept;

// Additive temporal experiment primitive; Prepare above remains independent RESET.
// Exactly ONE context/provider/default-query/configuration belongs to this session.
// Each Work has fresh converter textures and descriptor storage; there is no scratch
// reuse while prior GPU work is outstanding. Acyclic frame leases retain the shared
// context through existing final-consumer submission fences even if Session/Work dies.
// Inputs plus nominal private textures are capped at 512 MiB across retained session
// leases; provider-internal allocations and separately owned producer targets are not
// claimed to be included. Abandoned/failed-signal submissions remain charged/retained.
// Provider module management remains the caller's responsibility, as for Prepare.
std::shared_ptr<Session> CreateSession(ID3D12Device* device, const SessionDesc& description,
                                       const char** error = nullptr) noexcept;

// CPU preparation only, one pending frame slot, no hidden reset/fallback. Caller must
// supply true temporal parameters from its OWN previous accepted frame and the same
// view/origin. First frame explicitly has BOTH RESET flags; later frames have NEITHER
// and frameIndex is strictly previous+1 without wrap. Provider/settings/max extent and
// canonical queue are immutable. This is not a camera validator or a native view key.
// An accepted reservation that fails preparation/Record or whose Work is abandoned
// before acknowledgement permanently stops the session. A missing acknowledgement,
// busy slot, invalid admission or changed parameters refuses without reserving work.
// AcknowledgeExecuted supplies only ordering; ordinary submission fences still govern
// retirement. The host must veto failed consumers and all dependent work, restore
// engine state, and serialize other provider/module management as for Prepare.
std::shared_ptr<Work> PrepareFrame(const std::shared_ptr<Session>& session, const Parameters& parameters,
                                   const FrameAdmission& admission, const char** error = nullptr) noexcept;
} // namespace FSRD::PrivateDenoise

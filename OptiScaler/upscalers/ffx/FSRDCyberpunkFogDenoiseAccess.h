#pragma once

#include "FSRDCyberpunkEngineAccess.h"
#include <array>
#include <cstdint>
#include <limits>

namespace FSRD::CyberpunkFogDenoiseAccess
{
namespace Engine = CyberpunkEngineAccess;

// Reuse the exact native scope/getter/state/flush/reentry bodies. The transition
// queue helper is authenticated too; this does not authenticate every transitive
// driver/allocator target or protect against concurrent code patching.
inline constexpr std::array<Engine::CodeRange, 12> Code {
    Engine::Code[0], Engine::Code[1], Engine::Code[2], Engine::Code[3],
    Engine::Code[4], Engine::Code[5], Engine::Code[6], Engine::Code[7],
    Engine::Code[8], Engine::Code[9], Engine::Code[10], Engine::BufferCode[1] };
static_assert(Code[3].rva == Engine::RequestStateRva && Code[5].rva == Engine::FlushRva &&
              Code[9].rva == Engine::ReenterRva && Code[11].rva == 0x1f483c);
inline constexpr uint32_t CopySourceState = 0x800;

struct Input
{
    uintptr_t image = 0, list = 0, originalPso = 0;
    uint64_t originalFogScope = 0;
    Engine::TextureBorrow depth {}; // The actual current original Fog pixel t0, not material stencil.
    bool copySource = false; // Opt-in bitwise copy to owned private depth; default compute reads stay unchanged.
};

enum class Outcome : uint8_t
{
    Refused,                 // No depth state request or private recording occurred.
    PrivateFailedRestored,    // Callback false/throw; original bindings restored.
    PrivateRecordedRestored,  // Commands recorded/restored, NOT GPU completion.
    ScopeLostAfterMutation    // Fatal/unverified recording; do not blindly resume original draw.
};
struct Result
{
    Outcome outcome = Outcome::Refused;
    unsigned requestsIssued = 0;
    bool callbackEntered = false, bindingsRestored = false;
};

namespace Detail
{
struct Saved
{
    uintptr_t tls = 0, context = 0, cache = 0, registry = 0;
    uint32_t thread = 0, kind = 0;
};

template<class Host> bool SameScope(Host& host, const Input& input, const Saved& saved) noexcept
{
    try
    {
        uintptr_t tls = 0, context = 0, list = 0, cache = 0, pso = 0, registry = 0;
        uint8_t initialized = 0, masksStates = 0;
        uint32_t kind = 0;
        int32_t refs = 0;
        return host.ThreadId() == saved.thread && host.IsAdmittedFogDenoiseScope(input) &&
            host.ReadTlsSlotZero(tls) && tls == saved.tls &&
            Engine::Detail::Read(host, tls, 0x14, initialized) && initialized &&
            Engine::Detail::Read(host, tls, 0x188, context) && context == saved.context &&
            Engine::Detail::Read(host, context, 0x30, list) && list == input.list &&
            Engine::Detail::Read(host, context, 0x60, cache) && cache == saved.cache &&
            Engine::Detail::Read(host, context, 0x68, kind) && kind == saved.kind &&
            Engine::Detail::Read(host, context, 0x3d0, pso) && pso == input.originalPso &&
            Engine::Detail::Read(host, input.image, Engine::RegistryRva, registry) && registry == saved.registry &&
            (kind != 4 || (Engine::Detail::Read(host, registry, 0x1a8e988, masksStates) && !masksStates)) &&
            Engine::Detail::ReadTexture(host, registry, input.depth, refs);
        // ReadTexture requires positive refs, not an unchanged numeric count:
        // context-kind3 legitimately retains handles in the native state request.
    }
    catch (...) { return false; }
}

template<class Host> bool Prepare(Host& host, const Input& input, Saved& saved) noexcept
{
    try
    {
        if (!input.image || input.image > std::numeric_limits<uintptr_t>::max() - Engine::ImageBytes ||
            !input.list || !input.originalPso || !input.originalFogScope ||
            !input.depth.handle || input.depth.handle > 0x8000 || !input.depth.native ||
            !host.ExactImageAuthenticated(input.image, Engine::ImageBytes, Engine::ImageTimestamp, Engine::ExeSha256) ||
            !host.IsAdmittedFogDenoiseScope(input)) return false;
        for (const auto& code : Code)
            if (!host.LiveCodeMatches(input.image, code)) return false;
        // Exclude the lazy initialization branch before invoking the existing
        // native current-list getter. No getter is used to establish ownership.
        uint8_t initialized = 0;
        saved.thread = host.ThreadId();
        if (!saved.thread || !host.ReadTlsSlotZero(saved.tls) || !saved.tls ||
            !Engine::Detail::Read(host, saved.tls, 0x14, initialized) || !initialized ||
            !Engine::Detail::Read(host, saved.tls, 0x188, saved.context) || !saved.context ||
            !Engine::Detail::Read(host, saved.context, 0x60, saved.cache) || !saved.cache ||
            !Engine::Detail::Read(host, saved.context, 0x68, saved.kind) ||
            !Engine::Detail::Read(host, input.image, Engine::RegistryRva, saved.registry) || !saved.registry ||
            !SameScope(host, input, saved) || !host.ListIsDirect(input.list)) return false;
        return host.CurrentNativeList(input.image + Engine::GetCurrentListRva) == input.list &&
            SameScope(host, input, saved);
    }
    catch (...) { return false; }
}
} // namespace Detail

// Narrow host contract; no hooks, COM references, GPU state table or global-last
// resource substitution are installed by this header:
// - ExactImageAuthenticated/LiveCodeMatches provide full installed-image and exact
//   live-body evidence. Read/ReadTlsSlotZero are bounded CPU reads, not dereferences.
// - IsAdmittedFogDenoiseScope proves this synchronous original Fog draw's current
//   scope/view/list/Reset/camera and exact ACTIVE pixel t0 descriptor/resource borrow,
//   retained device/resource/descriptor lifetime, supported depth-plane/extent,
//   no main/writable/DSV alias, and no query/predication/render-pass/bundle ambiguity.
//   All other inputs are admitted private Color/guides/motion/hit resources, with
//   known read states at GPU use and producer→consumer ordering. They may be
//   produced earlier on this list, or by an owned private producer recording
//   whose exact dependency is enforced by a mandatory pre-Execute gate. CPU
//   callback order/allocation alone is never permission to consume them. Source
//   pointers, refs or an old graph snapshot do not establish this contract.
//   With copySource=true, the host also admits the exact native source format,
//   plane and copy extent and retains the private destination before recording.
// - That host gate repeats ORIGINAL-use receipts/current identities, not a claim
//   that the original native bindings remain installed during private compute.
//   Reentry legitimately dirties engine binding caches; do not require clean range
//   flags after it. The original engine context/cache/PSO identities remain fixed.
// - Optional IsTextureResidencyAdmitted must re-prove existing current membership
//   through the authenticated residency path; unknown/nonregistered paths refuse.
// - Native ABI adapters are noexcept: CurrentNativeList is void*(), RequestState
//   is void(context*,uint32 handle,uint32 state,uint32 subresource), Flush is
//   void(context*), Reenter is void(list*). RestorePso calls the real original
//   list SetPipelineState; it does not merely change an engine cache pointer.
// - privateWork is synchronous and PRIVATE-output-only, may change native compute
//   roots/heaps/PSO, and retains every recording owner before first use. It does
//   not change OM/RS, original resource states, engine memory/TLS/cache or call game
//   helpers. Include research shader copies in this callback before restoration;
//   copySource=true additionally permits native-source copies, without a shader,
//   using the engine-requested COPY_SOURCE state (no private source barrier).
// - Host must honor ScopeLostAfterMutation as an unusable/fatal recording. After
//   ordinary refusal/false it still executes the original Fog draw exactly once;
//   this helper does not forward that draw or authorize any scene color write.
template<class Host, class PrivateWork>
Result RecordPrivateCompute(Host& host, const Input& input, PrivateWork&& privateWork) noexcept
{
    static_assert(noexcept(host.CurrentNativeList(uintptr_t {})));
    static_assert(noexcept(host.RequestState(uintptr_t {}, uintptr_t {}, uint32_t {}, uint32_t {}, uint32_t {})));
    static_assert(noexcept(host.Flush(uintptr_t {}, uintptr_t {})));
    static_assert(noexcept(host.Reenter(uintptr_t {}, uintptr_t {})));
    static_assert(noexcept(host.RestorePso(uintptr_t {}, uintptr_t {})));
    Result result;
    Detail::Saved saved;
    if (!Detail::Prepare(host, input, saved)) return result;

    // The native helper owns StateBefore and first-use reconciliation. Combined
    // read preserves pixel access for the unchanged original Fog draw that follows.
    // Optional COPY_SOURCE is another read-only bit, not a guessed StateBefore.
    host.RequestState(input.image + Engine::RequestStateRva, saved.context, input.depth.handle,
                      Engine::InputReadState | (input.copySource ? CopySourceState : 0u), Engine::AllSubresources);
    ++result.requestsIssued;
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    host.Flush(input.image + Engine::FlushRva, saved.context);
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    result.callbackEntered = true;
    bool recorded = false;
    try { recorded = privateWork(); }
    catch (...) { /* Partial private recording still requires native binding restoration. */ }
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    // Reentry restores roots/heaps/tables, but not PSO and not cached PSO identity.
    // Check the original scope again before/after each step, including false/throw.
    host.Reenter(input.image + Engine::ReenterRva, input.list);
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    host.RestorePso(input.list, input.originalPso);
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    result.bindingsRestored = true;
    result.outcome = recorded ? Outcome::PrivateRecordedRestored : Outcome::PrivateFailedRestored;
    // Leave the engine-managed read-only superset truthful. No source undo barrier.
    return result;
}
} // namespace FSRD::CyberpunkFogDenoiseAccess

#pragma once

#include "FSRDCyberpunkEngineAccess.h"
#include "FSRDCyberpunkRayBindings.h"

#include <array>
#include <cstdint>
#include <limits>

namespace FSRD::CyberpunkRayAccess
{
namespace Engine = CyberpunkEngineAccess;
namespace Bindings = CyberpunkRayBindings;

// Exactly the unhooked native texture-state request, transition queue and flush
// bodies. Ray node/binder authentication belongs to the original-use host gate;
// those bodies may already contain the host's authenticated Detours.
inline constexpr std::array<Engine::CodeRange, 4> Code {
    Engine::Code[3], Engine::Code[4], Engine::BufferCode[1], Engine::Code[5] };
static_assert(Code[0].rva == 0x1f40dc && Code[1].rva == 0x1f41f8 &&
              Code[2].rva == 0x1f483c && Code[3].rva == 0x1f7164);
inline constexpr uint32_t CopySource = 0x800, HitUav = 8;

struct Input
{
    uintptr_t image = 0;
    Bindings::Snapshot dispatch; // Actual original t4/u0/u8 observation, not a synthesized binding.
};

enum class Outcome : uint8_t
{
    Refused,                 // No native state requests/private commands were made.
    CopyFailedRestored,       // False/throw from private callback; hit restored through the engine.
    CopyRecordedRestored,     // Commands recorded and hit restored, NOT GPU completion.
    ScopeLostAfterMutation   // Fatal/unverified recording: caller must not blindly resume the engine.
};
struct Result
{
    Outcome outcome = Outcome::Refused;
    unsigned requestsIssued = 0;
    bool callbackEntered = false, hitRestored = false;
};

namespace Detail
{
struct Saved
{
    uint32_t thread = 0, kind = 0;
};

inline Engine::TextureBorrow Borrow(const Bindings::TextureBinding& source)
{ return { source.handle, source.native }; }

template<class Host> bool SourceCurrent(Host& host, uintptr_t registry,
                                       const Bindings::TextureBinding& expected, bool uav)
{
    int32_t refs = 0;
    if (!Engine::Detail::ReadTexture(host, registry, Borrow(expected), refs)) return false;
    Bindings::TextureBinding current;
    if (!Bindings::Detail::ReadTexture(host, registry, expected.handle, uav, current)) return false;
    // Kind3 legitimately retains handles in RequestState. Require positive refs,
    // not an unchanged numeric count; no logical lifetime is inferred from refs.
    return current.slot == expected.slot && current.native == expected.native &&
        current.descriptor == expected.descriptor && current.compact == expected.compact &&
        current.requestedSrvState == expected.requestedSrvState &&
        current.extra == expected.extra && current.uavArray == expected.uavArray;
}

template<class Host> bool SameScope(Host& host, const Input& input, const Saved& saved) noexcept
{
    try
    {
        const auto& dispatch = input.dispatch;
        const auto& scope = dispatch.scope;
        uintptr_t tls = 0, list4 = 0, cache = 0, descriptor = 0, registry = 0;
        uint32_t kind = 0;
        uint8_t masksStates = 0;
        return host.ThreadId() == saved.thread && host.IsAdmittedPostRayScope(input) &&
            host.ReadTlsSlotZero(tls) && tls == scope.tls &&
            CyberpunkRayConstants::Detail::Current(host, scope, cache, descriptor) &&
            cache == dispatch.cache && descriptor == dispatch.b6.descriptor &&
            Engine::Detail::Read(host, scope.engine, 0x40, list4) && list4 == dispatch.list4 &&
            Engine::Detail::Read(host, scope.engine, 0x68, kind) && kind == saved.kind &&
            Engine::Detail::Read(host, input.image, Engine::RegistryRva, registry) && registry == dispatch.registry &&
            (kind != 4 || (Engine::Detail::Read(host, registry, 0x1a8e988, masksStates) && !masksStates)) &&
            SourceCurrent(host, registry, dispatch.textures[0], false) &&
            SourceCurrent(host, registry, dispatch.textures[2], true);
    }
    catch (...) { return false; }
}

template<class Host> bool Prepare(Host& host, const Input& input, Saved& saved) noexcept
{
    try
    {
        const auto& dispatch = input.dispatch;
        const auto& motion = dispatch.textures[0];
        const auto& hit = dispatch.textures[2];
        if (!input.image || input.image > std::numeric_limits<uintptr_t>::max() - Engine::ImageBytes ||
            !dispatch.list4 || !dispatch.cache || !dispatch.registry || !dispatch.b6.descriptor ||
            dispatch.callerRva != Bindings::DispatchReturnRva ||
            motion.handle == hit.handle || motion.native == hit.native ||
            motion.binding.shaderRegister != 4 || hit.binding.shaderRegister != 8 ||
            motion.binding.descriptor != motion.descriptor || hit.binding.descriptor != hit.descriptor ||
            (motion.requestedSrvState != 0x40 && motion.requestedSrvState != 0xc0) ||
            !host.ExactImageAuthenticated(input.image, Engine::ImageBytes, Engine::ImageTimestamp, Engine::ExeSha256))
            return false;
        for (const auto& code : Code)
            if (!host.LiveCodeMatches(input.image, code)) return false;
        saved.thread = host.ThreadId();
        if (!saved.thread || !Engine::Detail::Read(host, dispatch.scope.engine, 0x68, saved.kind) ||
            !SameScope(host, input, saved) || !host.ListIsDirect(dispatch.scope.list))
            return false;
        return SameScope(host, input, saved);
    }
    catch (...) { return false; }
}
} // namespace Detail

// Host contract, deliberately not installed here:
// - ExactImageAuthenticated and LiveCodeMatches supply full installed-image and
//   exact live body evidence. Read is bounded RPM, never unchecked pointer access.
// - IsAdmittedPostRayScope proves the original selected DispatchRays has returned
//   exactly once and this synchronous thread is still in that exact original ray
//   invocation, same graph/view/frame-source/List/List4/Reset generation. It also
//   revalidates original bind receipts/current descriptor use and actual owning
//   resource borrows, supported native formats/extent, no aliases, no predication,
//   query, render-pass/bundle ambiguity, and retained recording owners. A sampled
//   native pointer, positive refcount, or CPU b6 receipt alone cannot satisfy it.
// - ReadTlsSlotZero is bounded; no engine getter is invoked here.
// - Optional IsTextureResidencyAdmitted(registry, TextureBorrow) is reused by
//   EngineAccess::ReadTexture for nonzero slot+68. It must authenticate the native
//   residency path and re-prove ALREADY registered membership in this current
//   engine's open set; it must not infer membership from pointer equality alone.
// - RequestState and Flush MUST be noexcept adapters for the exact void native
//   ABI: (engine*, uint32 handle, uint32 state, uint32 subresource), then (engine*).
//   They continue the engine's trusted state/first-use reconciliation protocol.
//   There is no guessed StateBefore, redundant state-table tracker or GPU wait.
// - privateCopy is synchronous copy-only Work on this same list; it changes only
//   private destination states, never engine input states, roots, heaps, PSO/state
//   object, OM or caches. It retains recorded resources even on false/exception.
// - This function does not forward the original dispatch or resume game code.
//   The host MUST examine ScopeLostAfterMutation and latch an unusable recording;
//   it is not permission to continue without verified hit restoration.
// A primary-dispatch copy is not automatically the final authored hit version;
// shader/encoding/later-writer proof is separate from safe raw copying.
template<class Host, class PrivateCopy>
Result RecordCopy(Host& host, const Input& input, PrivateCopy&& privateCopy) noexcept
{
    static_assert(noexcept(host.RequestState(uintptr_t {}, uintptr_t {}, uint32_t {}, uint32_t {}, uint32_t {})));
    static_assert(noexcept(host.Flush(uintptr_t {}, uintptr_t {})));
    Result result;
    Detail::Saved saved;
    if (!Detail::Prepare(host, input, saved)) return result;
    const auto& dispatch = input.dispatch;
    const auto engine = dispatch.scope.engine;
    // The original authenticated bind/dispatch uses these same engine helpers.
    // Their void return is not a completion result; Flush records native barriers.
    host.RequestState(input.image + Engine::RequestStateRva, engine, dispatch.textures[0].handle,
                      dispatch.textures[0].requestedSrvState | CopySource, Engine::AllSubresources);
    ++result.requestsIssued;
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    host.RequestState(input.image + Engine::RequestStateRva, engine, dispatch.textures[2].handle,
                      CopySource, Engine::AllSubresources);
    ++result.requestsIssued;
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    host.Flush(input.image + Engine::FlushRva, engine);
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    result.callbackEntered = true;
    bool recorded = false;
    try { recorded = privateCopy(); }
    catch (...) { /* Every partial copy outcome still requires hit restoration. */ }
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result; // Never restore through a different/reused engine context.
    }
    host.RequestState(input.image + Engine::RequestStateRva, engine, dispatch.textures[2].handle,
                      HitUav, Engine::AllSubresources);
    ++result.requestsIssued;
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    host.Flush(input.image + Engine::FlushRva, engine);
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterMutation;
        return result;
    }
    result.hitRestored = true;
    result.outcome = recorded ? Outcome::CopyRecordedRestored : Outcome::CopyFailedRestored;
    // Motion stays in a truthful read-only COPY_SOURCE superset. Requesting the
    // smaller old read mask can be elided by1f42e0; do not invent an undo barrier.
    return result;
}
} // namespace FSRD::CyberpunkRayAccess

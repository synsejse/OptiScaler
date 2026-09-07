#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkEngineAccess
{
// Only for a synchronous private compute pass inside an already authenticated
// original engine draw scope on the executing thread (Fog or final lighting).
// The caller must admit compiler-reserved current resources, producer ordering,
// descriptor provenance and no active writable-target aliases BEFORE entry.
// Positive registry refs alone establish none of those facts.
inline constexpr std::string_view ExeSha256 =
    "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991";
inline constexpr uintptr_t ImageBytes = 0x04efc000;
inline constexpr uint32_t ImageTimestamp = 0x68af45ea;
inline constexpr uintptr_t RegistryRva = 0x3438a28;
inline constexpr uintptr_t GetCurrentListRva = 0x1f6400, RequestStateRva = 0x1f40dc,
                           FlushRva = 0x1f7164, ReenterRva = 0x2a3e9d4;
inline constexpr uintptr_t RequestBufferStateRva = 0x1f51c4;
// Preserve pixel readability if an input is also used by the intercepted draw:
// that raw DrawInstanced resumes AFTER its engine binding/transition setup.
inline constexpr uint32_t InputReadState = 0xc0; // NON_PIXEL | PIXEL_SHADER_RESOURCE
inline constexpr uint32_t LightingCaptureReadState = 0x8c0; // Also COPY_SOURCE, all read-only.
inline constexpr uint32_t AllSubresources = 0xffffffff;

struct CodeRange
{
    uintptr_t rva;
    size_t bytes;
    std::string_view sha256;
};

// Exact full bodies, not prologue signatures, from the SHA-authenticated 2.31
// executable. Unwind-table extents except two verified leaf bodies. None contains
// a base relocation. Authentication is of THESE bodies, not every transitive
// allocator/driver/COM target, nor a defense against concurrent hot-patching.
inline constexpr CodeRange Code[] = {
    { 0x1f405c, 0x33, "557e4e22128f0637e382d4c4ac3bd4232bf4249f28687e2b379334c0827fc0c1" },
    { 0x1f6400, 0x37, "2b07c2333edaeeb96892ad43be635d84eb081e3f63ddbdf0f147195401764c0f" },
    { 0x1ee438, 0x37, "a1afdb70f72542656f1f9c9da6bd498f140d8a40e5c0cf0d9cf790ce52a9b34b" },
    { 0x1f40dc, 0x11a, "622b528cd407dc30c63b1ffd46a1d2a4fbcc93d5a28887894eeb860b6d268a12" },
    { 0x1f41f8, 0x506, "ffe1a0c4ceaff9ec1c7d26621112f629b393adfd5a949f9b9c37f94d3868b3bd" },
    { 0x1f7164, 0x36, "9bcdcd41ce3c0aeed9cb99c1fc966cbf7cb526b2f83d5cc795d60f2800112724" },
    { 0x1f719c, 0x2d, "c6bb5bf48282e73f03b2415f4fb3448b0a7d854ff3d20db2fef15578897ec9a9" },
    { 0x1f71cc, 0x55, "b442be48c000b47da077b68c724ead031e73b07951d0cc18ff3d7412b02982df" },
    { 0x911bdc, 0x52, "9aff1eefdd9ce5809bf9d288d7672033b74fda9d8648c637536be6d4bb29ca4b" },
    { 0x2a3e9d4, 0x62, "5157e582dd180068fa1a8c9bc46614ab51858fdd55a8d14fdfcab56e4ae7a37b" },
    { 0xa1ffa8, 0x278, "2015eca47699d30f91c4c7719854975d10b8f8221c1a32fd8fc82183ca72ec18" }
};

// Authenticated only when the optional exposure buffer is present. The buffer
// request uses its own table at context+0x568; it is NOT the texture helper.
inline constexpr CodeRange BufferCode[] = {
    { 0x1f51c4, 0x180, "5fe9728672a33a381bddb7dce4757eb2a1581a002f4965d22fe75011666a3096" },
    { 0x1f483c, 0x196, "951414fc4b92797853094a3b16baa4c5e8c2f96347826b6dda80366baad9cb91" }
};

struct TextureBorrow
{
    uint32_t handle = 0;
    uintptr_t native = 0;
};

struct BufferBorrow
{
    uint32_t handle = 0;
    uintptr_t native = 0;
};

struct Input
{
    uintptr_t image = 0, list = 0, originalPso = 0;
    uint64_t originalFogScope = 0;
    std::array<TextureBorrow, 4> textures {}; // t0, t1, t2, t4; admitted by caller.
    // Narrow lighting-only opt-in: t4 is also the currently bound DSV with BOTH
    // depth and stencil read-only. Never accept a caller-selected arbitrary mask.
    bool preserveReadOnlyDepth = false;
    // Optional exact buffer used by BOTH original vertex/pixel t37. This is not
    // a generic buffer-read API; the host proves descriptor/format/current-use.
    BufferBorrow exposure {};
    // Optional original final-lighting pixel t8. Host proves actual current
    // binder/use and excludes all writable/DSV aliases. No arbitrary texture API.
    TextureBorrow lightingT8 {};
};

enum class Outcome
{
    Refused,                 // No engine state requests or private commands issued.
    ScopeLostBeforePrivate,  // State requests may be recorded; private callback did not run.
    PrivateFailedRestored,   // Callback failed/threw; engine binding state restored.
    PrivateRecordedRestored,// Callback succeeded and restored; NOT GPU completion.
    ScopeLostAfterPrivate    // FATAL to this recording: caller must not blindly resume raw draw.
};

struct Result
{
    Outcome outcome = Outcome::Refused;
    unsigned requestsIssued = 0;
    bool callbackEntered = false, bindingsRestored = false;
};

namespace Detail
{
inline bool HasExposure(const Input& input)
{
    // A partial nonzero pair is an invalid request, not an absent optional.
    return input.exposure.handle || input.exposure.native;
}

inline bool HasLightingT8(const Input& input)
{ return input.lightingT8.handle || input.lightingT8.native; }

template <typename Host> bool LightingT8Admitted(Host& host, const Input& input)
{
    if (!HasLightingT8(input)) return true;
    if constexpr (requires { host.IsLightingT8Admitted(input.lightingT8); })
        return host.IsLightingT8Admitted(input.lightingT8);
    return false;
}

template <typename Host> bool ExposureAdmitted(Host& host, const Input& input)
{
    if (!HasExposure(input)) return true;
    if constexpr (requires {
        host.IsExposureBufferAdmitted(input.exposure);
        host.RequestBufferState(uintptr_t {}, uintptr_t {}, uint32_t {}, uint32_t {});
    })
        return host.IsExposureBufferAdmitted(input.exposure);
    return false; // Existing hosts cannot silently opt into a missing contract.
}

template <typename Host> bool ReadOnlyDepthAdmitted(Host& host, const Input& input)
{
    if (!input.preserveReadOnlyDepth) return true;
    if constexpr (requires { host.IsReadOnlyDepthAliasAdmitted(input.textures[3]); })
        return host.IsReadOnlyDepthAliasAdmitted(input.textures[3]);
    return false; // Existing Fog hosts cannot opt into an unimplemented proof.
}
template <typename Host, typename T> bool Read(Host& host, uintptr_t base, uintptr_t offset, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    if (!base || offset > Max - base || sizeof(T) - 1 > Max - (base + offset))
        return false;
    return host.Read(base + offset, &value, sizeof(value));
}

template <typename Host> bool ReadExposure(Host& host, uintptr_t registry, const BufferBorrow& buffer,
                                         int32_t& refs)
{
    if (!buffer.handle || buffer.handle > 0x8000 || !buffer.native) return false;
    // Buffer table capacity/stride: constructor91b6f9..91b711. Refcount is
    // incremented by21c937; native resource is used by1f5307/1f0a01/1f0aca.
    const uintptr_t slot = 0x5c0af0 + uintptr_t(buffer.handle - 1) * 0xb0;
    uintptr_t native = 0, externalSynchronization = 0;
    uint8_t kinds = 0;
    if (!Read(host, registry, slot, refs) || refs <= 0 ||
        !Read(host, registry, slot + 0x18, native) || native != buffer.native ||
        !Read(host, registry, slot + 0xe, kinds) ||
        !Read(host, registry, slot + 0x70, externalSynchronization) || externalSynchronization)
        return false;
    const auto memoryKind = kinds >> 4;
    const auto viewKind = kinds & 0xf;
    // Refuse dynamic kind4 and the1/2/5 branches that skip ordinary state
    // requests. Admit only known state-managed kinds, never an unknown enum.
    // The selected low kinds take the structured SRV factory branch221542.
    return (memoryKind == 0 || memoryKind == 3 || memoryKind == 6) &&
        (viewKind == 8 || viewKind == 9 || viewKind == 10 || viewKind == 14);
}

struct Snapshot
{
    uintptr_t tls = 0, context = 0, cache = 0, registry = 0;
    uint32_t thread = 0, kind = 0;
    std::array<int32_t, 4> refs {};
    int32_t exposureRefs = 0;
    int32_t lightingT8Refs = 0;
};

template <typename Host> bool ReadTexture(Host&, uintptr_t, const TextureBorrow&, int32_t&);

template <typename Host> bool SameScope(Host& host, const Input& input, const Snapshot& saved)
{
    uintptr_t tls = 0, context = 0, list = 0, cache = 0, pso = 0, registry = 0;
    uint8_t initialized = 0;
    uint32_t kind = 0;
    int32_t exposureRefs = 0;
    int32_t lightingT8Refs = 0;
    return host.ThreadId() == saved.thread &&
        ReadOnlyDepthAdmitted(host, input) &&
        ExposureAdmitted(host, input) &&
        LightingT8Admitted(host, input) &&
        host.IsAdmittedFogScope(input.originalFogScope, input.list, input.originalPso, input.textures) &&
        host.ReadTlsSlotZero(tls) && tls == saved.tls &&
        Read(host, tls, 0x14, initialized) && initialized &&
        Read(host, tls, 0x188, context) && context == saved.context &&
        Read(host, context, 0x30, list) && list == input.list &&
        Read(host, context, 0x60, cache) && cache == saved.cache &&
        Read(host, context, 0x68, kind) && kind == saved.kind &&
        Read(host, context, 0x3d0, pso) && pso == input.originalPso &&
        Read(host, input.image, RegistryRva, registry) && registry == saved.registry &&
        (!HasExposure(input) || (ReadExposure(host, registry, input.exposure, exposureRefs) &&
                                 exposureRefs == saved.exposureRefs)) &&
        (!HasLightingT8(input) || (ReadTexture(host, registry, input.lightingT8, lightingT8Refs) &&
                                   lightingT8Refs == saved.lightingT8Refs));
}

template <typename Host> bool ReadTexture(Host& host, uintptr_t registry, const TextureBorrow& texture,
                                        int32_t& refs)
{
    if (!texture.handle || texture.handle > 0x8000 || !texture.native)
        return false;
    const uintptr_t slot = 0x2f1d8 + uintptr_t(texture.handle - 1) * 0xb0;
    uintptr_t native = 0, residencyResource = 0;
    uint8_t flags = 0;
    if (!(Read(host, registry, slot - 8, refs) && refs > 0 &&
        Read(host, registry, slot, native) && native == texture.native &&
        // The skip-state branch remains unsupported. Nonzero +0x68 is the
        // managed residency object's underlying resource, not a fence.
        Read(host, registry, slot + 0x56, flags) && !(flags & 0x40) &&
        Read(host, registry, slot + 0x68, residencyResource)))
        return false;
    if (!residencyResource) return true;
    if (residencyResource != texture.native) return false;
    // Only a host with exact current original-use and already-registered
    // residency proof may opt in. Legacy adapters retain the old refusal.
    if constexpr (requires { host.IsTextureResidencyAdmitted(registry, texture); })
        return host.IsTextureResidencyAdmitted(registry, texture);
    return false;
}

template <typename Host> bool Prepare(Host& host, const Input& input, Snapshot& saved)
{
    if (!input.image || input.image > std::numeric_limits<uintptr_t>::max() - ImageBytes ||
        !input.list || !input.originalPso || !input.originalFogScope ||
        !host.ExactImageAuthenticated(input.image, ImageBytes, ImageTimestamp, ExeSha256) ||
        !host.IsAdmittedFogScope(input.originalFogScope, input.list, input.originalPso, input.textures))
        return false;
    for (const auto& code : Code)
        if (!host.LiveCodeMatches(input.image, code))
            return false;
    if (!LightingT8Admitted(host, input)) return false;
    if (HasExposure(input))
    {
        if (!ExposureAdmitted(host, input)) return false;
        for (const auto& code : BufferCode)
            if (!host.LiveCodeMatches(input.image, code)) return false;
    }
    // ReadTlsSlotZero clones only gs:[0x58] -> slot0 using bounded reads.
    // Never call the engine getter until its initialization branch is excluded.
    uint8_t initialized = 0;
    saved.thread = host.ThreadId();
    if (!saved.thread || !host.ReadTlsSlotZero(saved.tls) ||
        !Read(host, saved.tls, 0x14, initialized) || !initialized ||
        !Read(host, saved.tls, 0x188, saved.context) || !saved.context ||
        !Read(host, saved.context, 0x60, saved.cache) || !saved.cache ||
        !Read(host, saved.context, 0x68, saved.kind) ||
        !Read(host, input.image, RegistryRva, saved.registry) || !saved.registry ||
        (HasExposure(input) && !ReadExposure(host, saved.registry, input.exposure, saved.exposureRefs)) ||
        (HasLightingT8(input) && !ReadTexture(host, saved.registry, input.lightingT8, saved.lightingT8Refs)) ||
        !SameScope(host, input, saved))
        return false;
    // Internal kind is NOT D3D12_COMMAND_LIST_TYPE. Reject only the known
    // active masking branch that would strip PIXEL_SHADER_RESOURCE from0xc0.
    if (saved.kind == 4)
    {
        uint8_t masksStates = 0;
        if (!Read(host, saved.registry, 0x1a8e988, masksStates) || masksStates)
            return false;
    }
    for (size_t i = 0; i < input.textures.size(); ++i)
        if (!ReadTexture(host, saved.registry, input.textures[i], saved.refs[i]))
            return false;
    for (size_t i = 0; i < input.textures.size(); ++i)
    {
        int32_t refs = 0;
        if (!ReadTexture(host, saved.registry, input.textures[i], refs) || refs != saved.refs[i])
            return false;
    }
    if (!SameScope(host, input, saved) || !host.ListIsDirect(input.list))
        return false;
    return host.CurrentNativeList(input.image + GetCurrentListRva) == input.list &&
        SameScope(host, input, saved);
}
} // namespace Detail

// Host contract (platform adapter intentionally not installed here):
// - All methods except privateWork are noexcept; Read is bounded RPM, never raw
//   unchecked dereference. LiveCodeMatches checks the entire MEM_IMAGE executable
//   range belongs to input.image and hashes LIVE bytes against CodeRange::sha256.
// - ExactImageAuthenticated supplies existing full-file SHA/PE identity evidence.
// - IsAdmittedFogScope validates synchronous scope/list/PSO/resource admission,
//   including logical reservations, producer order and no writable aliases.
// - The optional IsReadOnlyDepthAliasAdmitted validates the current input[3]
//   native resource against the exact bound DSV and proves BOTH read-only flags.
//   It is rechecked at each scope gate when preserveReadOnlyDepth is true.
// - Optional IsExposureBufferAdmitted proves exact original VS AND PS t37 use,
//   authored structured SRV/28-byte stride, descriptor ownership/range, current
//   native BUFFER extent, no writable alias, and retained same-scope lifetime.
//   It is rechecked at each gate together with the buffer registry fields.
// - Optional IsLightingT8Admitted proves exact original final-lighting PS t8
//   binder/descriptor/current-resource ownership and no target/DSV aliases.
//   It receives a fixed COPY_SOURCE|NON_PIXEL|PIXEL read request. Native state
//   may remain a compatible superset; subsequent diagnostic readback must NOT
//   emit a source barrier with a guessed exact StateBefore/restore value.
// - Optional IsTextureResidencyAdmitted must authenticate the original native
//   residency helper and prove this original-use resource already belongs to
//   the current list's open residency set. Pointer equality alone is not proof.
//   It is rechecked with each ReadTexture; no implicit insertion is admitted.
// - CurrentNativeList invokes the authenticated Win64 void*() getter.
// - RequestState invokes void(context*, uint32 handle, uint32 nativeState,
//   uint32 subresource); Flush invokes void(context*); Reenter invokes void(list*).
// - Optional RequestBufferState invokes void(context*, uint32 handle,
//   uint32 nativeState). Its fixed0xc0 preserves original VS+PS readability;
//   it does NOT admit CopyBufferRegion reads from the engine resource.
// - RestorePso calls native list->SetPipelineState(originalPso), NOT a cache write.
// - privateWork performs only synchronous native compute recording on this list;
//   it must not change TLS/engine caches/graphics state or invoke engine helpers.
//   Its private resources/PSO/descriptors survive submission independently.
// No queues, fences, engine-memory writes, COM ownership or global-last fallback
// are invented here. Engine state helpers themselves mutate the actual tracker
// and may retain handles according to the engine's own context-kind policy.
template <typename Host, typename PrivateWork>
Result RecordPrivateCompute(Host& host, const Input& input, PrivateWork&& privateWork) noexcept
{
    Result result;
    Detail::Snapshot saved;
    if (!Detail::Prepare(host, input, saved))
        return result;
    // All admission checks precede any mutation. The engine owns StateBefore,
    // unknown-first-use reconciliation and later submission. Flush is NOT a wait.
    for (size_t i = 0; i < input.textures.size(); ++i)
    {
        const auto& texture = input.textures[i];
        const auto readState = InputReadState | ((i == 3 && input.preserveReadOnlyDepth) ? 0x20u : 0u);
        host.RequestState(input.image + RequestStateRva, saved.context, texture.handle, readState, AllSubresources);
        ++result.requestsIssued;
    }
    if (Detail::HasExposure(input))
    {
        if constexpr (requires {
            host.RequestBufferState(uintptr_t {}, uintptr_t {}, uint32_t {}, uint32_t {});
        })
        {
            host.RequestBufferState(input.image + RequestBufferStateRva, saved.context,
                                    input.exposure.handle, InputReadState);
            ++result.requestsIssued;
        }
        // Missing method was already refused before any mutation in Prepare.
    }
    if (Detail::HasLightingT8(input))
    {
        host.RequestState(input.image + RequestStateRva, saved.context, input.lightingT8.handle,
                          LightingCaptureReadState, AllSubresources);
        ++result.requestsIssued;
    }
    host.Flush(input.image + FlushRva, saved.context);
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostBeforePrivate;
        return result;
    }
    result.callbackEntered = true;
    bool recorded = false;
    try { recorded = privateWork(); }
    catch (...) { /* Partial native recording still requires reentry. */ }
    if (!Detail::SameScope(host, input, saved))
    {
        result.outcome = Outcome::ScopeLostAfterPrivate;
        return result; // Never call TLS-based reentry on a different context.
    }
    // SDK reentry restores engine roots/heaps/tables + dirties binding caches,
    // but DOES NOT restore native PSO or invalidate cached PSO at context+0x3d0.
    host.Reenter(input.image + ReenterRva, input.list);
    host.RestorePso(input.list, input.originalPso);
    result.bindingsRestored = true;
    result.outcome = recorded ? Outcome::PrivateRecordedRestored : Outcome::PrivateFailedRestored;
    // Leave engine-tracked combined read states truthful, not guessed undo barriers.
    return result;
}
} // namespace FSRD::CyberpunkEngineAccess

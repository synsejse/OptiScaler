#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkEngineAccess
{
// Isolated, NOT installed/invoked. Only for a synchronous private compute pass
// inside an already authenticated original Fog draw on the executing thread.
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
// Preserve pixel readability if an input is also used by the intercepted draw:
// that raw DrawInstanced resumes AFTER its engine binding/transition setup.
inline constexpr uint32_t InputReadState = 0xc0; // NON_PIXEL | PIXEL_SHADER_RESOURCE
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

struct TextureBorrow
{
    uint32_t handle = 0;
    uintptr_t native = 0;
};

struct Input
{
    uintptr_t image = 0, list = 0, originalPso = 0;
    uint64_t originalFogScope = 0;
    std::array<TextureBorrow, 4> textures {}; // t0, t1, t2, t4; admitted by caller.
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
template <typename Host, typename T> bool Read(Host& host, uintptr_t base, uintptr_t offset, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    if (!base || offset > Max - base || sizeof(T) - 1 > Max - (base + offset))
        return false;
    return host.Read(base + offset, &value, sizeof(value));
}

struct Snapshot
{
    uintptr_t tls = 0, context = 0, cache = 0, registry = 0;
    uint32_t thread = 0, kind = 0;
    std::array<int32_t, 4> refs {};
};

template <typename Host> bool SameScope(Host& host, const Input& input, const Snapshot& saved)
{
    uintptr_t tls = 0, context = 0, list = 0, cache = 0, pso = 0, registry = 0;
    uint8_t initialized = 0;
    uint32_t kind = 0;
    return host.ThreadId() == saved.thread &&
        host.IsAdmittedFogScope(input.originalFogScope, input.list, input.originalPso, input.textures) &&
        host.ReadTlsSlotZero(tls) && tls == saved.tls &&
        Read(host, tls, 0x14, initialized) && initialized &&
        Read(host, tls, 0x188, context) && context == saved.context &&
        Read(host, context, 0x30, list) && list == input.list &&
        Read(host, context, 0x60, cache) && cache == saved.cache &&
        Read(host, context, 0x68, kind) && kind == saved.kind &&
        Read(host, context, 0x3d0, pso) && pso == input.originalPso &&
        Read(host, input.image, RegistryRva, registry) && registry == saved.registry;
}

template <typename Host> bool ReadTexture(Host& host, uintptr_t registry, const TextureBorrow& texture,
                                        int32_t& refs)
{
    if (!texture.handle || texture.handle > 0x8000 || !texture.native)
        return false;
    const uintptr_t slot = 0x2f1d8 + uintptr_t(texture.handle - 1) * 0xb0;
    uintptr_t native = 0, externalSynchronization = 0;
    uint8_t flags = 0;
    return Read(host, registry, slot - 8, refs) && refs > 0 &&
        Read(host, registry, slot, native) && native == texture.native &&
        // Unsupported paths: +0x56 bit6 skips state requests, +0x68 invokes
        // the separate synchronization helper. Do not claim either is handled.
        Read(host, registry, slot + 0x56, flags) && !(flags & 0x40) &&
        Read(host, registry, slot + 0x68, externalSynchronization) && !externalSynchronization;
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
// - CurrentNativeList invokes the authenticated Win64 void*() getter.
// - RequestState invokes void(context*, uint32 handle, uint32 nativeState,
//   uint32 subresource); Flush invokes void(context*); Reenter invokes void(list*).
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
    for (const auto& texture : input.textures)
    {
        host.RequestState(input.image + RequestStateRva, saved.context, texture.handle,
                          InputReadState, AllSubresources);
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

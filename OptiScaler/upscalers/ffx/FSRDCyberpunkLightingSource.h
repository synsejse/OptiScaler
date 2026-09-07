#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkLightingSource
{
// Narrow no-call reader for the authenticated154610 final-lighting t8 owner
// route. Actual binder receipt and pixel consumption are SEPARATE caller proof.
// No sampled pointer is dereferenced as COM, and no signal semantics are guessed.
inline constexpr uintptr_t BinderRva = 0x1f3a6c;
inline constexpr uintptr_t BindSixReturnRva = 0x155cc0, BindFiveReturnRva = 0x155d24;
struct CodeRange
{
    uintptr_t rva;
    size_t bytes;
    std::string_view sha256;
};
// Additional live bodies the caller authenticates before permitting a nonzero
// residency underlying pointer. These do not replace the existing source hashes.
inline constexpr CodeRange Code[] = {
    { 0x1f5a28, 0x94, "1b6370b8815f17ac7f788969ac3aa2fb79d58f66a1483109f28d563db70dc3c3" },
    { 0x1fe694, 0x76, "e5592c3cd3c8353c1dc06f8689c0dca725cce76fee26198aed2e5d83ca90943b" },
    { 0x1fe5f4, 0x9f, "e6b6e54ae6e337a7769093d7ac64a9eed9dcf3e23b0476eed2ee3361d30cf2e7" }
};
inline constexpr uint32_t ResidencyIndices = 100; // Exact native Open bound.
inline constexpr int32_t MaxResidencyScan = 4096; // Diagnostic cap, not engine maximum.
inline constexpr int32_t MaxResidencyCapacity = 1024 * 1024; // Diagnostic cap.
struct ResidencySnapshot
{
    uintptr_t engineContext = 0, set = 0, object = 0, underlying = 0;
    uintptr_t array = 0, allocator = 0, memberAddress = 0;
    uint64_t objectBytes = 0;
    uint32_t index = 0;
    int32_t capacity = 0, count = 0;
    uint8_t open = 0, outOfMemory = 0, reserved = 0;
    // Deliberately no whole bitset word: unrelated recording lists may change
    // other bits atomically without invalidating this list's membership.
    bool memberBit = false;
    bool operator==(const ResidencySnapshot&) const = default;
};
struct Snapshot
{
    uintptr_t context = 0, view = 0, owner = 0, registry = 0, slot = 0;
    uint64_t viewFlags = 0;
    uint32_t handle = 0, requestedState = 0;
    int32_t refs = 0;
    // Historical field name retained for callers; +68 is the inline residency
    // object's underlying pageable pointer, NOT a fence/synchronization object.
    uintptr_t native = 0, descriptor = 0, externalSync = 0;
    std::array<uint8_t, 12> compact {};
    ResidencySnapshot residency {};
    bool operator==(const Snapshot&) const = default;
};

template <typename Host>
bool ReadBytes(Host& host, uintptr_t base, uintptr_t offset, void* result, size_t bytes)
{
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    return base && bytes && offset <= Max - base && bytes - 1 <= Max - (base + offset) &&
        host.Read(base + offset, result, bytes);
}

template <typename Host, typename T>
bool Read(Host& host, uintptr_t base, uintptr_t offset, T& result)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return ReadBytes(host, base, offset, &result, sizeof(T));
}

// No engine calls, COM access, allocation or mutation. The caller supplies the
// authenticated current TLS engine context for this same active original draw,
// and repeats the full source + residency observation at engine-access gates.
// Only ALREADY registered membership is accepted: native Insert then re-ORs
// this existing bit and returns before its allocation/append path.
template <typename Host>
bool ObserveRegisteredResidency(Host& host, uintptr_t engineContext, const Snapshot& source,
                                ResidencySnapshot& output) noexcept
{
    output = {};
    try
    {
        ResidencySnapshot s; s.engineContext = engineContext;
        if (!source.native || source.externalSync != source.native || !source.slot ||
            source.slot > UINTPTR_MAX - 0x98)
            return false;
        s.object = source.slot + 0x60;
        uintptr_t native = 0;
        if (!Read(host, source.slot, 0, native) || native != source.native ||
            !Read(host, s.object, 8, s.underlying) || s.underlying != source.native ||
            !Read(host, s.object, 0x10, s.objectBytes) || !s.objectBytes ||
            !Read(host, engineContext, 0x630, s.set) || !s.set ||
            !Read(host, s.set, 0, s.index) || s.index >= ResidencyIndices ||
            !Read(host, s.set, 8, s.array) || !s.array ||
            !Read(host, s.set, 0x10, s.capacity) || s.capacity <= 0 || s.capacity > MaxResidencyCapacity ||
            !Read(host, s.set, 0x20, s.count) || s.count <= 0 || s.count > s.capacity || s.count > MaxResidencyScan ||
            !Read(host, s.set, 0x24, s.open) || s.open != 1 ||
            !Read(host, s.set, 0x25, s.outOfMemory) || s.outOfMemory ||
            !Read(host, s.set, 0x28, s.allocator) || !s.allocator ||
            !Read(host, s.allocator, 0x28 + s.index, s.reserved) || s.reserved != 1)
            return false;
        const uintptr_t wordOffset = 0x28 + 8 * uintptr_t(s.index >> 6);
        const uint64_t mask = uint64_t(1) << (s.index & 63);
        uint64_t word = 0;
        if (!Read(host, s.object, wordOffset, word) || !(word & mask)) return false;
        s.memberBit = true;
        const uintptr_t arrayBytes = uintptr_t(s.count) * sizeof(uintptr_t);
        if (arrayBytes - 1 > UINTPTR_MAX - s.array) return false;
        std::array<uintptr_t, 64> members {};
        for (int32_t first = 0; first < s.count;)
        {
            const size_t count = size_t(s.count - first) < members.size() ? size_t(s.count - first) : members.size();
            if (!ReadBytes(host, s.array, uintptr_t(first) * sizeof(uintptr_t), members.data(), count * sizeof(uintptr_t)))
                return false;
            for (size_t i = 0; i < count; ++i)
                if (members[i] == s.object)
                {
                    if (s.memberAddress) return false; // Native Insert deduplicates.
                    s.memberAddress = s.array + (uintptr_t(first) + i) * sizeof(uintptr_t);
                }
            first += int32_t(count);
        }
        if (!s.memberAddress) return false;
        // Check the selected entry/header again after the bounded scan. This
        // excludes a closed/reused set; it is not a cross-thread atomic snapshot.
        uintptr_t set = 0, array = 0, allocator = 0, object = 0, underlying = 0;
        uint32_t index = 0; int32_t count = 0, capacity = 0;
        uint8_t open = 0, oom = 0, reserved = 0;
        if (!Read(host, engineContext, 0x630, set) || set != s.set ||
            !Read(host, s.set, 0, index) || index != s.index ||
            !Read(host, s.set, 8, array) || array != s.array ||
            !Read(host, s.set, 0x10, capacity) || capacity != s.capacity ||
            !Read(host, s.set, 0x20, count) || count != s.count ||
            !Read(host, s.set, 0x24, open) || open != 1 ||
            !Read(host, s.set, 0x25, oom) || oom ||
            !Read(host, s.set, 0x28, allocator) || allocator != s.allocator ||
            !Read(host, s.allocator, 0x28 + s.index, reserved) || reserved != 1 ||
            !Read(host, s.memberAddress, 0, object) || object != s.object ||
            !Read(host, s.object, 8, underlying) || underlying != s.underlying ||
            !Read(host, s.object, wordOffset, word) || !(word & mask))
            return false;
        output = s;
        return true;
    }
    catch (...) { return false; }
}

inline bool IsFinalBind(uintptr_t image, uintptr_t caller, uint32_t first, uint32_t count, uint8_t stage)
{
    return image && image <= UINTPTR_MAX - BindFiveReturnRva && first == 5 && stage == 1 &&
        ((caller == image + BindSixReturnRva && count == 6) ||
         (caller == image + BindFiveReturnRva && count == 5));
}

// Caller authenticates original lighting/binder/factory bodies (LightingCode),
// current executing scope and shader, then repeats exact Snapshot equality.
// Only native owner+26c is supported: no graph/alternate/default fallback.
template <typename Host>
bool Observe(Host& host, uintptr_t image, uintptr_t context, Snapshot& output,
             uintptr_t engineContext = 0) noexcept
{
    output = {};
    try
    {
        Snapshot s; s.context = context;
        uint8_t executing = 0;
        if (!image || image > UINTPTR_MAX - 0x04efc000 ||
            !Read(host, context, 0x30, executing) || !(executing & 2) ||
            !Read(host, context, 0x18, s.view) || !s.view ||
            !Read(host, s.view, 0x17d0, s.viewFlags) || !(s.viewFlags & (uint64_t(1) << 51)) ||
            (s.viewFlags & (uint64_t(1) << 55)) ||
            !Read(host, s.view, 0x1d70, s.owner) || !s.owner ||
            !Read(host, s.owner, 0x26c, s.handle) || !s.handle || s.handle > 0x8000 ||
            !Read(host, image, 0x3438a28, s.registry) || !s.registry)
            return false;
        const uintptr_t offset = 0x2f1d8 + uintptr_t(s.handle - 1) * 0xb0;
        if (offset > UINTPTR_MAX - s.registry) return false;
        s.slot = s.registry + offset;
        if (!Read(host, s.slot - 8, 0, s.refs) || s.refs <= 0 ||
            !Read(host, s.slot, 0, s.native) || !s.native ||
            !Read(host, s.slot, 0x30, s.descriptor) || !s.descriptor ||
            !Read(host, s.slot, 0x48, s.requestedState) || s.requestedState != 0xc0 ||
            !Read(host, s.slot, 0x4e, s.compact) ||
            !Read(host, s.slot, 0x68, s.externalSync))
            return false;
        // Authenticated ordinary Texture2D/mip0/plane0 SRV factory. Tag11
        // resolves to RGBA16_FLOAT; host still checks actual GetDesc/heap.
        if (s.compact[4] != 1 || s.compact[5] != 0 || (s.compact[6] & 0xf) != 0 ||
            s.compact[7] != 0x11 || !(s.compact[8] & 1) || (s.compact[8] & 4) ||
            (s.compact[8] & 0x40))
            return false;
        if (s.externalSync && !ObserveRegisteredResidency(host, engineContext, s, s.residency))
            return false;
        output = s;
        return true;
    }
    catch (...) { return false; }
}
} // namespace FSRD::CyberpunkLightingSource

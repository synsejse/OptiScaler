#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkLightingConstants
{
inline constexpr uintptr_t UploadRva = 0xad2254, UploadReturnRva = 0x1554a1;
inline constexpr uintptr_t NativeCbvReturnRva = 0x1f01a6;
inline constexpr size_t PayloadBytes = 240;
inline constexpr uint32_t NativeCbvBytes = 256;
struct CodeRange { uintptr_t rva; size_t bytes; std::string_view sha256; };
// Host authenticates the complete file and these complete LIVE bodies, in
// addition to the existing final lighting callback/fullscreen/draw manifest.
inline constexpr CodeRange Code[] = {
    { 0xad2254, 0x65, "4652d592bf5ba2bbc88efda799ae3d8d2a6e177a81cf18320f91725c3506919a" },
    { 0x1f0114, 0xad, "02643fa55c85da8c0d4e60f80940bfda58bb378cd52d2c237485ccca3dc9ccdc" },
    { 0x1f3978, 0xf1, "4bec13e036ce264fcebad667a02d9b45fe1f84e6bac71b7c90729dd900c65274" },
    { 0x1f405c, 0x33, "557e4e22128f0637e382d4c4ac3bd4232bf4249f28687e2b379334c0827fc0c1" },
    { 0x1ee438, 0x37, "a1afdb70f72542656f1f9c9da6bd498f140d8a40e5c0cf0d9cf790ce52a9b34b" },
    { 0x1f22e4, 0x4dc, "f4a5782e0cead125409e02ce0ff209aa5468564dc286cd1403798a1bb18f8bfc" }
};

struct Scope
{
    uint64_t serial = 0, recordingGeneration = 0;
    uintptr_t graphContext = 0, view = 0, tls = 0, engine = 0, list = 0;
    bool operator==(const Scope&) const = default;
};
enum class Phase : uint8_t { Empty, Pending, Uploaded, Invalid };
struct Receipt
{
    Scope scope;
    uintptr_t image = 0, source = 0, cache = 0, descriptor = 0;
    uint64_t bufferLocation = 0;
    std::array<uint32_t, PayloadBytes / 4> words {};
    Phase phase = Phase::Empty;
    bool nativeDescriptorWriteObserved = false;
};
struct Binding
{
    uintptr_t cache = 0, layout = 0, descriptorArray = 0, descriptor = 0;
    uint32_t descriptorIndex = 0;
    uint8_t rangeIndex = 0, nativeRootParameter = 0;
    std::array<uint8_t, 16> range {};
    bool operator==(const Binding&) const = default;
};

inline void Invalidate(Receipt& receipt) noexcept
{
    receipt = {};
    receipt.phase = Phase::Invalid;
}

namespace Detail
{
inline bool Address(uintptr_t base, uintptr_t offset, size_t bytes, uintptr_t& result) noexcept
{
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    if (!base || !bytes || offset > Max - base || bytes - 1 > Max - (base + offset)) return false;
    result = base + offset;
    return true;
}
template<class Host, class T> bool Read(Host& host, uintptr_t base, uintptr_t offset, T& output)
{
    static_assert(std::is_trivially_copyable_v<T>);
    uintptr_t address = 0;
    return Address(base, offset, sizeof(output), address) && host.Read(address, &output, sizeof(output));
}
template<class Host> bool Current(Host& host, const Scope& scope, uintptr_t& cache, uintptr_t& descriptor)
{
    if (!scope.serial || !scope.recordingGeneration || !scope.graphContext || !scope.view ||
        !scope.tls || !scope.engine || !scope.list) return false;
    uint8_t initialized = 0;
    uintptr_t engine = 0, list = 0, view = 0;
    return Read(host, scope.tls, 0x14, initialized) && initialized &&
        Read(host, scope.tls, 0x188, engine) && engine == scope.engine &&
        Read(host, scope.engine, 0x30, list) && list == scope.list &&
        Read(host, scope.graphContext, 0x18, view) && view == scope.view &&
        Read(host, scope.engine, 0x60, cache) && cache &&
        Read(host, scope.engine, 0xa0, descriptor) && descriptor;
}
} // namespace Detail

// Native ABI: ad2254 consumes only UINT byteCount (ECX), const void* data (RDX).
// The observer never calls it. Host always invokes original exactly once and
// calls Complete while the original caller's source stack is still alive.
// Host supplies its actual current TLS/scope/list-generation identity, not a
// previously sampled one; Current reads are corroboration, not TLS discovery.
template<class Host>
bool Begin(Host& host, uintptr_t image, uintptr_t caller, const Scope& scope,
           uint32_t byteCount, uintptr_t source, Receipt& output) noexcept
{
    output = {};
    try
    {
        Receipt value;
        uintptr_t expectedCaller = 0;
        if (!Detail::Address(image, UploadReturnRva, 1, expectedCaller) || caller != expectedCaller ||
            byteCount != PayloadBytes || !Detail::Current(host, scope, value.cache, value.descriptor) ||
            !Detail::Read(host, source, 0, value.words)) return false;
        value.scope = scope; value.image = image; value.source = source; value.phase = Phase::Pending;
        output = value;
        return true;
    }
    catch (...) { return false; }
}

// Optional stronger witness from ORIGINAL ID3D12Device::CreateConstantBufferView
// calls. Host must observe ALL writes to this one descriptor through final draw
// (including another thread/no lighting scope), synchronize this receipt, and
// invalidate on an observation gap. A descriptor integer alone is reusable.
// No descriptor memory, GPU VA, COM object or engine function is dereferenced.
inline void DescriptorWrite(Receipt& receipt, const Scope& observedScope, uintptr_t caller,
                            uintptr_t descriptor, uint64_t bufferLocation, uint32_t bytes) noexcept
{
    if (receipt.phase == Phase::Empty || receipt.phase == Phase::Invalid || descriptor != receipt.descriptor) return;
    uintptr_t expectedCaller = 0;
    if (receipt.phase != Phase::Pending || receipt.nativeDescriptorWriteObserved ||
        observedScope != receipt.scope ||
        !Detail::Address(receipt.image, NativeCbvReturnRva, 1, expectedCaller) || caller != expectedCaller ||
        !bufferLocation || (bufferLocation & 255) || bytes != NativeCbvBytes ||
        bytes - 1 > std::numeric_limits<uint64_t>::max() - bufferLocation)
    {
        Invalidate(receipt);
        return;
    }
    receipt.nativeDescriptorWriteObserved = true;
    receipt.bufferLocation = bufferLocation;
}

template<class Host>
bool Complete(Host& host, const Scope& scope, Receipt& receipt) noexcept
{
    try
    {
        uintptr_t cache = 0, descriptor = 0;
        std::array<uint32_t, PayloadBytes / 4> words {};
        if (receipt.phase == Phase::Pending && receipt.scope == scope &&
            Detail::Current(host, scope, cache, descriptor) && cache == receipt.cache && descriptor == receipt.descriptor &&
            Detail::Read(host, receipt.source, 0, words) && words == receipt.words)
        {
            receipt.source = 0; // Never retain the caller's stack pointer beyond this hook.
            receipt.phase = Phase::Uploaded;
            return true; // CPU upload receipt; DescriptorWrite is separately reported.
        }
    }
    catch (...) {}
    Invalidate(receipt);
    return false;
}

// Pure bounded observation of the actual pixel CBV register6 cache route. This
// is a DESCRIPTOR TABLE route, not SetGraphicsRootConstantBufferView. As with
// the existing SRV reader, 65536 is a diagnostic index cap, not engine capacity.
// Host authenticates exact current final draw + PSO shader/selector, repeats
// this result, and disallows Reset/scope changes or intervening descriptor writes.
template<class Host>
bool ObserveBound(Host& host, const Scope& scope, uintptr_t pso, const Receipt& receipt,
                  Binding& output) noexcept
{
    output = {};
    try
    {
        Binding value;
        uintptr_t descriptor = 0, cachedPso = 0;
        if (receipt.phase != Phase::Uploaded || receipt.scope != scope || !pso ||
            !Detail::Current(host, scope, value.cache, descriptor) || value.cache != receipt.cache ||
            descriptor != receipt.descriptor || !Detail::Read(host, scope.engine, 0x3d0, cachedPso) || cachedPso != pso ||
            !Detail::Read(host, value.cache, 0x68, value.layout) || !value.layout ||
            !Detail::Read(host, value.cache, 0x28, value.descriptorArray) || !value.descriptorArray ||
            !Detail::Read(host, value.layout, 0x453 + 0x1c + 2 * 6, value.rangeIndex) || value.rangeIndex >= 64 ||
            !Detail::Read(host, value.layout, 0x38 + 0x10 * uintptr_t(value.rangeIndex), value.range)) return false;
        uint64_t resourceMask = 0, samplerMask = 0, dirty70 = 0, dirty78 = 0;
        const uint64_t bit = uint64_t(1) << value.rangeIndex;
        if (!Detail::Read(host, value.layout, 8, resourceMask) || !(resourceMask & bit) ||
            !Detail::Read(host, value.layout, 0, samplerMask) || (samplerMask & bit) ||
            !Detail::Read(host, value.cache, 0x70, dirty70) || !Detail::Read(host, value.cache, 0x78, dirty78) ||
            ((dirty70 | dirty78) & bit)) return false;
        uint32_t base = 0;
        uint16_t first = 0, count = 0;
        std::memcpy(&base, value.range.data() + 4, 4);
        std::memcpy(&first, value.range.data() + 8, 2);
        std::memcpy(&count, value.range.data() + 10, 2);
        if (first > 6 || !count || 6 - first >= count || value.range[13] == 2 || value.range[14] >= 64) return false;
        const uint64_t index = uint64_t(base) + 6 - first;
        if (index >= 65536 || !Detail::Read(host, value.descriptorArray, uintptr_t(index) * 8, value.descriptor) ||
            value.descriptor != receipt.descriptor) return false;
        value.descriptorIndex = uint32_t(index);
        value.nativeRootParameter = value.range[14];
        output = value;
        return true; // Correspondence, not proof of unwitnessed descriptor contents.
    }
    catch (...) { return false; }
}

// Used only when the host kept DescriptorWrite coverage live without gaps.
// This does not retain the GPU allocation or authorize mapping/copying its VA.
inline bool HasNativeDescriptorWitness(const Receipt& receipt) noexcept
{
    return receipt.phase == Phase::Uploaded && receipt.nativeDescriptorWriteObserved && receipt.bufferLocation;
}
} // namespace FSRD::CyberpunkLightingConstants

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkRayConstants
{
inline constexpr uintptr_t NodeRva = 0xc6a3b4, UploadRva = 0x1ee3cc;
inline constexpr std::array<uintptr_t, 2> UploadReturnRvas { 0xc6bb70, 0xc6bbd6 };
inline constexpr size_t PayloadBytes = 0xcd0;
inline constexpr size_t EncodingByteOffset = 1212, WriteHitByteOffset = 3168;
inline constexpr uintptr_t EncodingConfigByteRva = 0x32f7798;
struct CodeRange { uintptr_t rva; size_t bytes; std::string_view sha256; };
// Authenticate the complete installed image and complete LIVE bodies before installing
// any observer. Node ABI: void(node, GraphContext*); upload ABI: void(UINT, const void*).
// This header performs neither authentication nor engine calls; the host owns both hooks.
inline constexpr CodeRange Code[] = {
    { NodeRva, 0x1a37, "c2af94096758c595d46b6f4437676290076599d379774bbe541ab83ca6da435d" },
    { 0xae5718, 0x1df8, "694e9c4403d2e54146ce3686b59a7bc313b5bf2e6948719aab41b0d173b2124d" },
    { UploadRva, 0x6a, "ccdb0de963ae6139fb151d7b0428e341f7150b2a751d336059316ef09a358e67" },
    { 0x1f0114, 0xad, "02643fa55c85da8c0d4e60f80940bfda58bb378cd52d2c237485ccca3dc9ccdc" },
    { 0x1f3978, 0xf1, "4bec13e036ce264fcebad667a02d9b45fe1f84e6bac71b7c90729dd900c65274" },
    { 0x1f405c, 0x33, "557e4e22128f0637e382d4c4ac3bd4232bf4249f28687e2b379334c0827fc0c1" },
    { 0x1ee438, 0x37, "a1afdb70f72542656f1f9c9da6bd498f140d8a40e5c0cf0d9cf790ce52a9b34b" }
};

struct Scope
{
    uint64_t serial = 0, recordingGeneration = 0;
    uintptr_t graphContext = 0, view = 0, tls = 0, engine = 0, list = 0;
    uint32_t frameSource = 0; // Observed CPU source; zero is valid, not a late/GPU frame proof.
    bool operator==(const Scope&) const = default;
};
enum class Phase : uint8_t { Empty, Pending, Uploaded, Invalid };
struct Receipt
{
    Scope scope;
    uintptr_t source = 0, callerRva = 0, cache = 0, descriptor = 0;
    std::array<uint32_t, PayloadBytes / 4> words {};
    Phase phase = Phase::Empty;
};
inline void Invalidate(Receipt& receipt) noexcept
{
    receipt = {};
    receipt.phase = Phase::Invalid;
}

namespace Detail
{
template<class Host, class T> bool Read(Host& host, uintptr_t base, uintptr_t offset, T& output)
{
    static_assert(std::is_trivially_copyable_v<T>);
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    return base && offset <= Max - base && sizeof(T) - 1 <= Max - (base + offset) &&
        host.Read(base + offset, &output, sizeof(output));
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
        Read(host, scope.engine, 0x90, descriptor) && descriptor;
}
} // namespace Detail

// Copy original upload bytes before the host calls original1ee3cc exactly once.
// Supplied scope must be the actual current TLS/node/list-generation snapshot.
// Invalidate on every later upload (including unmatched calls) and nested node entry.
// Do not retain a caller stack pointer unless Pending, within this synchronous hook.
template<class Host>
bool Begin(Host& host, uintptr_t image, uintptr_t caller, const Scope& scope,
           uint32_t bytes, uintptr_t source, Receipt& output) noexcept
{
    output = {};
    try
    {
        Receipt value;
        if (!image || caller < image || bytes != PayloadBytes) return false;
        value.callerRva = caller - image;
        if ((value.callerRva != UploadReturnRvas[0] && value.callerRva != UploadReturnRvas[1]) ||
            !Detail::Current(host, scope, value.cache, value.descriptor) ||
            !Detail::Read(host, source, 0, value.words)) return false;
        value.scope = scope; value.source = source; value.phase = Phase::Pending;
        output = value;
        return true;
    }
    catch (...) { return false; }
}

template<class Host>
bool Complete(Host& host, const Scope& scope, Receipt& receipt) noexcept
{
    try
    {
        uintptr_t cache = 0, descriptor = 0;
        std::array<uint32_t, PayloadBytes / 4> words {};
        if (receipt.phase == Phase::Pending && receipt.scope == scope &&
            Detail::Current(host, scope, cache, descriptor) && cache == receipt.cache &&
            descriptor == receipt.descriptor && Detail::Read(host, receipt.source, 0, words) && words == receipt.words)
        {
            receipt.source = 0;
            receipt.phase = Phase::Uploaded;
            return true; // CPU upload receipt only; no current GPU payload or dispatch association claim.
        }
    }
    catch (...) {}
    Invalidate(receipt);
    return false;
}
} // namespace FSRD::CyberpunkRayConstants

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkExposureSource
{
inline constexpr std::string_view ExeSha256 =
    "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991";
inline constexpr uintptr_t ImageBytes = 0x04efc000;
inline constexpr uint32_t ImageTimestamp = 0x68af45ea;
inline constexpr uintptr_t RegistryRva = 0x3438a28, GlobalHandleRva = 0x3495d48;
inline constexpr uintptr_t GetterRva = 0x18ec810;
inline constexpr std::array<uint8_t, 5> GetterBytes { 0x48, 0x8d, 0x41, 0x10, 0xc3 };
inline constexpr uint32_t MaxBufferBytes = 1024 * 1024;

struct CodeRange
{
    uintptr_t rva;
    size_t bytes;
    std::string_view sha256;
};

// Caller authenticates these full LIVE image bodies before Observe. No hashing
// callback, allocation, engine call or interpretation of sampled COM pointers
// occurs here. Getter target and its exact leaf bytes are additionally checked
// by Observe whenever the selector takes that virtual route.
inline constexpr CodeRange Code[] = {
    { 0x775268, 0x47, "b5b35f8fb1d25688c122237d06b893311af67e50973b3f9bc5d19632b7b12095" },
    { 0x7752b0, 0x3d, "2ee91797e0a8ab2905702ef68d00e82eb41a9a7df1551967e7eb00be8a0ce084" },
    { 0x7752f0, 0x32, "60864169fac46bf557335c04298bcab1e216583106383acf2cb80c56a201c771" },
    { 0x1f5438, 0x143, "f5534e77727359d534f3dc7b95135aea0441642be9ec244a52dc5357d864ff96" },
    { 0x2213bc, 0x445, "3f38e3f5fb907f109d0275bc849c07aacf6ee39d502c2132141a56ea45d9bea3" }
};

enum class Route : uint8_t
{
    None,
    GlobalNoObject,
    GlobalNoModeOwner,
    GlobalModeDisabled,
    GlobalNoExposureOwner,
    ViewOwner
};

struct Snapshot
{
    uintptr_t image = 0, graphContext = 0;
    uintptr_t object = 0, vtable = 0, getterTarget = 0, modeOwner = 0;
    std::array<uint8_t, 5> getterBytes {};
    uint32_t mode = 0;
    uintptr_t view = 0, contextSecondary = 0, exposureOwner = 0;
    Route route = Route::None;
    uintptr_t selectedHandleAddress = 0;
    uint32_t handle = 0;
    uintptr_t registry = 0, slot = 0, native = 0, descriptor = 0;
    int32_t refCount = 0;
    std::array<uint8_t, 8> compact {};
    uint32_t byteCount = 0;
    uint16_t stride = 0;
    uint8_t memoryKind = 0, viewKind = 0;
    uintptr_t externalSynchronization = 0;

    bool operator==(const Snapshot&) const = default;
};

namespace Detail
{
inline bool Address(uintptr_t base, uintptr_t offset, size_t bytes, uintptr_t& address)
{
    constexpr auto Max = std::numeric_limits<uintptr_t>::max();
    if (!base || !bytes || offset > Max - base || bytes - 1 > Max - (base + offset)) return false;
    address = base + offset;
    return true;
}

template <typename Host, typename T> bool Read(Host& host, uintptr_t base, uintptr_t offset, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    uintptr_t address = 0;
    return Address(base, offset, sizeof(value), address) && host.Read(address, &value, sizeof(value));
}

inline uint16_t U16(const std::array<uint8_t, 8>& bytes, size_t offset)
{
    return uint16_t(uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8));
}
inline uint32_t U32(const std::array<uint8_t, 8>& bytes, size_t offset)
{
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
        (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}
} // namespace Detail

// Host::Read(address, destination, size) supplies a bounded byte read. This
// clones only 775268's CPU selector and the ordinary buffer registry metadata.
// Caller owns full-file/live-body authentication, exact current native draw,
// repeated Snapshot equality, actual VS+PS t37 descriptor matches, COM/heap
// ownership and native extent/state checks. A successful Snapshot establishes
// none of those independently and must not authorize a deferred pointer use.
// Failure clears output, including when a caller's Read unexpectedly throws.
template <typename Host>
bool Observe(Host& host, uintptr_t image, uintptr_t graphContext, Snapshot& output) noexcept
{
    output = {};
    try
    {
        Snapshot value;
        uintptr_t checkedImageBase = 0;
        if (!Detail::Address(image, 0, ImageBytes, checkedImageBase) || !graphContext) return false;
        value.image = image;
        value.graphContext = graphContext;
        if (!Detail::Read(host, graphContext, 0, value.object)) return false;

        if (!value.object)
            value.route = Route::GlobalNoObject;
        else
        {
            if (!Detail::Read(host, value.object, 0, value.vtable) ||
                !Detail::Read(host, value.vtable, 0x20, value.getterTarget) ||
                value.getterTarget != image + GetterRva ||
                !Detail::Read(host, value.getterTarget, 0, value.getterBytes) || value.getterBytes != GetterBytes ||
                !Detail::Read(host, value.object, 0xfb0, value.modeOwner))
                return false;
            if (!value.modeOwner)
                value.route = Route::GlobalNoModeOwner;
            else
            {
                if (!Detail::Read(host, value.modeOwner, 0x10, value.mode)) return false;
                if (value.mode == 2 || value.mode == 3)
                    value.route = Route::GlobalModeDisabled;
                else
                {
                    // Native 775268 takes its error branch for either missing
                    // context field here; that is NOT its global fallback.
                    if (!Detail::Read(host, graphContext, 0x18, value.view) || !value.view ||
                        !Detail::Read(host, graphContext, 0x20, value.contextSecondary) || !value.contextSecondary ||
                        !Detail::Read(host, value.view, 0x1d80, value.exposureOwner))
                        return false;
                    value.route = value.exposureOwner ? Route::ViewOwner : Route::GlobalNoExposureOwner;
                }
            }
        }
        if (!Detail::Address(value.route == Route::ViewOwner ? value.exposureOwner : image,
                             value.route == Route::ViewOwner ? 0x18 : GlobalHandleRva,
                             sizeof(value.handle), value.selectedHandleAddress) ||
            !Detail::Read(host, value.selectedHandleAddress, 0, value.handle) ||
            !value.handle || value.handle > 0x8000)
            return false; // A zero/invalid owner-selected value never falls back.

        if (!Detail::Read(host, image, RegistryRva, value.registry) ||
            !Detail::Address(value.registry, 0x5c0af0 + uintptr_t(value.handle - 1) * 0xb0, 0xb0, value.slot) ||
            !Detail::Read(host, value.slot, 0, value.refCount) || value.refCount <= 0 ||
            !Detail::Read(host, value.slot, 8, value.compact) ||
            !Detail::Read(host, value.slot, 0x18, value.native) || !value.native ||
            !Detail::Read(host, value.slot, 0x50, value.descriptor) || !value.descriptor ||
            !Detail::Read(host, value.slot, 0x70, value.externalSynchronization) || value.externalSynchronization)
            return false;
        value.byteCount = Detail::U32(value.compact, 0);
        value.stride = Detail::U16(value.compact, 4);
        value.memoryKind = value.compact[6] >> 4;
        value.viewKind = value.compact[6] & 0xf;
        // Exclude dynamic kind 4 and transition-skipping kinds 1/2/5. These are
        // engine kinds, not invented mappings onto public D3D12 heap enums.
        if ((value.memoryKind != 0 && value.memoryKind != 3 && value.memoryKind != 6) ||
            (value.viewKind != 8 && value.viewKind != 9 && value.viewKind != 10 && value.viewKind != 14) ||
            value.stride != 28 || value.byteCount < 28 || value.byteCount > MaxBufferBytes)
            return false;
        // Factory 221542 uses UNKNOWN-format BUFFER, stride and floor(size/stride).
        // These exact view kinds cannot take 1f7eb8's suballocation route, so its
        // first element is 0. The actual native descriptor is still caller proof.
        output = value;
        return true;
    }
    catch (...) { return false; }
}
} // namespace FSRD::CyberpunkExposureSource

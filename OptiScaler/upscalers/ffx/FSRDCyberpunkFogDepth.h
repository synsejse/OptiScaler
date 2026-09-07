#pragma once

#include "FSRDCyberpunkEngineAccess.h"

#include <cstring>

namespace FSRD::CyberpunkFogDepth
{
using CodeRange = CyberpunkEngineAccess::CodeRange;
inline constexpr uintptr_t GraphKey = 0xdebf0c27;
inline constexpr uintptr_t BindReturnRva = 0x61fbf5, FullscreenReturnRva = 0x61fc4f,
                           DrawReturnRva = 0x20ccac;
inline constexpr uint32_t MaxDescriptorIndex = 65536; // Diagnostic cap, NOT engine array capacity.

// Authenticate before installing any detour in these bodies. The admitted
// tag1b format path remains within the main20ce78 leaf (no cold branch).
// These hashes do not replace exact Fog shader/PSO and scoped caller admission.
inline constexpr CodeRange Code[] = {
    { 0x61f9e0, 0x403, "be3aea974eaa9d0be9abcbbab0691a477b6caf545b27c2783a9dacca2e95b71d" },
    { 0x1f3a6c, 0x2b1, "3d8ea951900012b9cb82212dc4d84a01312eac865475cb2e86501cdb138e1b1b" },
    { 0x20c954, 0x63, "dcc4dcd485f4318d8708181a715d6c4b26619a9c8523648525821a49dec92180" },
    { 0x20cc64, 0x58, "4833392e71c08768cbd9f77675121fa58bc2ff9f8fb27080386226d5cf13dca0" },
    { 0x1f22e4, 0x4dc, "f4a5782e0cead125409e02ce0ff209aa5468564dc286cd1403798a1bb18f8bfc" },
    { 0x2221f4, 0xc38, "a42b9e7ead94a67cd1a3dc7e405614ec4eeeadb9b955c0db9ea817f6a56b0562" },
    { 0x20ce78, 0x233, "d602022ad50044bb15eb32562cb0bedb8edb18395b3193e2fe77b9941fbb5c2a" }
};

struct Scope
{
    uint64_t serial = 0, recordingGeneration = 0;
    uintptr_t graphContext = 0, view = 0, tls = 0, engine = 0, list = 0, pso = 0;
    bool operator==(const Scope&) const = default;
};
struct Snapshot
{
    Scope scope;
    uintptr_t image = 0, registry = 0, slot = 0, native = 0, descriptor = 0, alternateDescriptor = 0;
    uintptr_t residencyUnderlying = 0, cache = 0, layout = 0, descriptorArray = 0, mapAddress = 0;
    uint32_t handle = 0, requestedSrvState = 0, descriptorIndex = 0;
    int32_t refs = 0;
    std::array<uint8_t, 12> compact {};
    std::array<uint8_t, 16> range {};
    uint8_t rangeIndex = 0, rootParameter = 0;
    uint16_t width = 0, height = 0;
    // Authenticated authored SRV fields, NOT an observed native GetDesc result.
    uint32_t srvFormat = 0, srvDimension = 0, planeSlice = 0, mostDetailedMip = 0,
             srvMipLevels = 0, componentMapping = 0;
    bool operator==(const Snapshot&) const = default;
};
enum class Failure : uint8_t
{
    None, Caller, CurrentScope, TextureSource, UnsupportedView, BindingRange, DescriptorMismatch, Changed, ReadException
};
inline constexpr std::string_view FailureName(Failure value) noexcept
{
    switch (value)
    {
    case Failure::None: return "observed";
    case Failure::Caller: return "not_admitted_original_fog_draw";
    case Failure::CurrentScope: return "current_fog_scope_or_cache_unavailable";
    case Failure::TextureSource: return "selected_depth_registry_source_unavailable";
    case Failure::UnsupportedView: return "unsupported_authored_hardware_depth_view";
    case Failure::BindingRange: return "pixel_t0_resource_range_unavailable_or_not_flushed";
    case Failure::DescriptorMismatch: return "pixel_t0_differs_from_selected_ordinary_depth_srv";
    case Failure::Changed: return "selected_depth_binding_snapshot_changed";
    case Failure::ReadException: return "bounded_memory_read_threw";
    }
    return "invalid_failure";
}
inline bool IsDepthBind(uintptr_t image, uintptr_t caller, uint32_t first, uint32_t count, uint8_t stage) noexcept
{
    return image && caller >= image && caller - image == BindReturnRva && first == 0 && count == 1 && stage == 1;
}

namespace Detail
{
using CyberpunkEngineAccess::Detail::Read;
inline uint16_t U16(const std::array<uint8_t, 12>& bytes, size_t offset)
{ return uint16_t(bytes[offset]) | uint16_t(uint16_t(bytes[offset + 1]) << 8); }

template<class Host>
bool Current(Host& host, uintptr_t image, const Scope& scope, uint32_t handle, Snapshot& value, Failure& failure)
{
    uintptr_t view = 0, engine = 0, list = 0, pso = 0;
    uint8_t initialized = 0;
    failure = Failure::CurrentScope;
    if (!Read(host, scope.graphContext, 0x18, view) || view != scope.view ||
        !Read(host, scope.tls, 0x14, initialized) || !initialized ||
        !Read(host, scope.tls, 0x188, engine) || engine != scope.engine ||
        !Read(host, engine, 0x30, list) || list != scope.list ||
        !Read(host, engine, 0x3d0, pso) || pso != scope.pso ||
        !Read(host, engine, 0x60, value.cache) || !value.cache ||
        !Read(host, value.cache, 0x68, value.layout) || !value.layout ||
        !Read(host, value.cache, 0x28, value.descriptorArray) || !value.descriptorArray ||
        !Read(host, image, CyberpunkEngineAccess::RegistryRva, value.registry) || !value.registry) return false;
    value.scope = scope; value.image = image; value.handle = handle;
    failure = Failure::TextureSource;
    if (!handle || handle > 32768) return false;
    const auto offset = uintptr_t(0x2f1d8) + uintptr_t(handle - 1) * 0xb0;
    if (!Read(host, value.registry, offset - 8, value.refs) || value.refs <= 0 ||
        !Read(host, value.registry, offset, value.native) || !value.native ||
        !Read(host, value.registry, offset + 0x30, value.descriptor) || !value.descriptor ||
        !Read(host, value.registry, offset + 0x38, value.alternateDescriptor) ||
        !Read(host, value.registry, offset + 0x48, value.requestedSrvState) ||
        !Read(host, value.registry, offset + 0x4e, value.compact) ||
        !Read(host, value.registry, offset + 0x68, value.residencyUnderlying)) return false;
    value.slot = value.registry + offset;
    value.width = U16(value.compact, 0); value.height = U16(value.compact, 2);
    failure = Failure::UnsupportedView;
    // Exactly the observed ordinary single-plane depth route. Excludes material
    // stencil tags18/19, arrays/MSAA/multi-mip and unmanaged-state textures.
    if (!value.width || !value.height || U16(value.compact, 4) != 1 || value.compact[6] != 0x10 ||
        value.compact[7] != 0x1b || (value.compact[8] & 5) != 5 || (value.compact[8] & 0x40) ||
        value.alternateDescriptor || value.requestedSrvState != 0xe0) return false;
    // 20ce78(tag1b)=DXGI_R32_FLOAT. Factory2222b6 zeroes the full SRV,
    // selects Texture2D/mip0/plane0/default mapping and all mips (one here).
    value.srvFormat = 41; value.srvDimension = 4; value.srvMipLevels = 0xffffffff;
    value.componentMapping = 0x1688;

    failure = Failure::BindingRange;
    uint64_t resources = 0, samplers = 0, dirty70 = 0, dirty78 = 0;
    if (!Read(host, value.layout, 0x5c3, value.rangeIndex) || value.rangeIndex >= 64 ||
        !Read(host, value.layout, 0x38 + uintptr_t(value.rangeIndex) * 16, value.range) ||
        !Read(host, value.layout, 8, resources) || !Read(host, value.layout, 0, samplers) ||
        !Read(host, value.cache, 0x70, dirty70) || !Read(host, value.cache, 0x78, dirty78)) return false;
    const auto bit = uint64_t(1) << value.rangeIndex;
    if (!(resources & bit) || (samplers & bit) || ((dirty70 | dirty78) & bit) ||
        value.range[13] == 2 || value.range[14] >= 64) return false;
    uint16_t first = 0, count = 0;
    std::memcpy(&value.descriptorIndex, value.range.data() + 4, 4);
    std::memcpy(&first, value.range.data() + 8, 2);
    std::memcpy(&count, value.range.data() + 10, 2);
    // t0 requires first==0; hence no potentially wrapping base+register delta.
    if (first || !count || value.descriptorIndex >= MaxDescriptorIndex) return false;
    uintptr_t bound = 0;
    if (!Read(host, value.descriptorArray, uintptr_t(value.descriptorIndex) * 8, bound) || !bound) return false;
    failure = Failure::DescriptorMismatch;
    if (bound != value.descriptor) return false;
    value.mapAddress = value.layout + 0x5c3; value.rootParameter = value.range[14];
    return true;
}
} // namespace Detail

// Caller supplies CURRENT initialized TLS (no initializing getter), exact nested
// Fog/fullscreen/native draw scope, tracked list Reset generation, matching PSO,
// and depth handle from existing EarlyGuides::Describe(GraphKey). It must repeat
// graph selection and independently acquire input/CPU-heap ownership at original
// use. This reader proves only bounded CPU correspondence: refs and registry
// residency fields do NOT prove ownership, graph lifetime, GPU state or ordering.
// In particular requestedSrvState=e0 is not an observed current native state.
template<class Host>
bool Observe(Host& host, uintptr_t image, uintptr_t caller, const Scope& scope, uint32_t handle,
             Snapshot& output, Failure* reason = nullptr) noexcept
{
    output = {};
    Failure failure = Failure::Caller;
    bool success = false;
    try
    {
        constexpr auto Max = std::numeric_limits<uintptr_t>::max();
        if (image && image <= Max - CyberpunkEngineAccess::ImageBytes && caller >= image &&
            caller - image == DrawReturnRva && scope.serial && scope.recordingGeneration &&
            scope.graphContext && scope.view && scope.tls && scope.engine && scope.list && scope.pso)
        {
            Snapshot first, second;
            if (Detail::Current(host, image, scope, handle, first, failure) &&
                Detail::Current(host, image, scope, handle, second, failure))
            {
                failure = Failure::Changed;
                if (first == second) { output = first; failure = Failure::None; success = true; }
            }
        }
    }
    catch (...) { failure = Failure::ReadException; }
    if (reason) *reason = failure;
    return success;
}
} // namespace FSRD::CyberpunkFogDepth

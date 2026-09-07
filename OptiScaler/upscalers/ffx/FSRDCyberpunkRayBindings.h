#pragma once

#include "FSRDCyberpunkRayConstants.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace FSRD::CyberpunkRayBindings
{
using CodeRange = CyberpunkRayConstants::CodeRange;
using Scope = CyberpunkRayConstants::Scope;
using Receipt = CyberpunkRayConstants::Receipt;
inline constexpr uintptr_t DispatchReturnRva = 0x2a4087f;
inline constexpr uint32_t TextureSlots = 32768, MaxDescriptorIndex = 65536;
// The index cap is a diagnostic read bound, NOT the engine array's capacity.
// Authenticate these live bodies plus RayConstants::Code before installing the
// actual ID3D12GraphicsCommandList4::DispatchRays observer. Observe itself only
// performs bounded CPU reads. The caller supplies an exact current ray scope,
// independently resolved graph/owner handles, and the actual native API caller.
inline constexpr CodeRange Code[] = {
    { 0x2934058, 0xf6, "e4d70b8d43eccdad33045a63c9ebe640439acf451e8efc54b28b5f54eb3992d5" },
    { 0x2a406e4, 0x1bc, "4fcbce8ff1f1b8c1073d0bd8a53c1dcccf5a28c23744c1297e289b5f629bd316" },
    { 0x1f22e4, 0x4dc, "f4a5782e0cead125409e02ce0ff209aa5468564dc286cd1403798a1bb18f8bfc" },
    { 0x1f3a6c, 0x2b1, "3d8ea951900012b9cb82212dc4d84a01312eac865475cb2e86501cdb138e1b1b" },
    { 0x153f94, 0x16c, "07bb021d4bb3001a5a036442bb326a7d31c724cf9d3f8dd596da47a6727350e7" }
};

struct TextureHandles
{
    uint32_t motion = 0, radiance = 0, hit = 0;
};
struct Binding
{
    uint32_t shaderRegister = 0, descriptorIndex = 0;
    uintptr_t mapAddress = 0, descriptor = 0;
    std::array<uint8_t, 16> range {};
    uint8_t rangeIndex = 0, rootParameter = 0;
    bool operator==(const Binding&) const = default;
};
struct TextureBinding
{
    uint32_t handle = 0, requestedSrvState = 0;
    int32_t refs = 0;
    uintptr_t slot = 0, native = 0, descriptor = 0, extra = 0, uavArray = 0;
    std::array<uint8_t, 12> compact {};
    Binding binding;
    bool operator==(const TextureBinding&) const = default;
};
struct Snapshot
{
    Scope scope;
    uintptr_t callerRva = 0, list4 = 0, cache = 0, layout = 0, descriptorArray = 0, registry = 0;
    Binding b6;
    std::array<TextureBinding, 3> textures {}; // Original motion t4, radiance u0, hit u8.
    bool operator==(const Snapshot&) const = default;
};
// Diagnostic copies only, populated when two complete CPU reads disagree,
// including accepted positive refcount changes. They never authorize recording.
struct ChangedSnapshots
{
    Snapshot first {}, second {};
    bool available = false;
};

// The authenticated engine retains/releases handles concurrently (native
// 21c992 atomically increments slot-8). That count is not resource identity.
// Require it to remain positive in BOTH reads, and compare every other field
// exactly. Original-use lifetime, owned resource and state gates remain separate.
// Preserve the actual sampled counts; normalize only these local comparison copies.
inline bool SameBindingIdentity(Snapshot first, Snapshot second) noexcept
{
    for (size_t i = 0; i < first.textures.size(); ++i)
    {
        if (first.textures[i].refs <= 0 || second.textures[i].refs <= 0) return false;
        first.textures[i].refs = second.textures[i].refs = 0;
    }
    return first == second;
}
enum class Failure : uint8_t
{
    None, Caller, UploadReceipt, CurrentScope, TextureSource, BindingRange, DescriptorMismatch, Changed, ReadException
};
inline constexpr std::string_view FailureName(Failure failure) noexcept
{
    switch (failure)
    {
    case Failure::None: return "observed";
    case Failure::Caller: return "not_exact_native_dispatch_caller";
    case Failure::UploadReceipt: return "no_matching_original_b6_upload_receipt";
    case Failure::CurrentScope: return "current_ray_scope_or_cache_unavailable";
    case Failure::TextureSource: return "authored_texture_descriptor_source_unavailable";
    case Failure::BindingRange: return "compute_resource_range_unavailable_or_not_flushed";
    case Failure::DescriptorMismatch: return "actual_binding_differs_from_authored_source";
    case Failure::Changed: return "selected_binding_snapshot_changed";
    case Failure::ReadException: return "bounded_memory_read_threw";
    }
    return "invalid_failure";
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
template<class Host> bool ReadBinding(Host& host, Snapshot& value, uintptr_t mapOffset,
                                      uint32_t reg, uintptr_t expected, Binding& output, Failure& failure)
{
    uint8_t rangeIndex = 0xff;
    uint64_t resourceMask = 0, samplerMask = 0, dirty70 = 0, dirty78 = 0;
    failure = Failure::BindingRange;
    if (!Read(host, value.layout, mapOffset, rangeIndex) || rangeIndex >= 64 ||
        !Read(host, value.layout, 0x38 + uintptr_t(rangeIndex) * 16, output.range) ||
        !Read(host, value.layout, 8, resourceMask) || !Read(host, value.layout, 0, samplerMask) ||
        !Read(host, value.cache, 0x70, dirty70) || !Read(host, value.cache, 0x78, dirty78)) return false;
    const auto bit = uint64_t(1) << rangeIndex;
    if (!(resourceMask & bit) || (samplerMask & bit) || ((dirty70 | dirty78) & bit) ||
        output.range[13] != 2 || output.range[14] >= 64) return false;
    uint32_t base = 0;
    uint16_t first = 0, count = 0;
    std::memcpy(&base, output.range.data() + 4, sizeof(base));
    std::memcpy(&first, output.range.data() + 8, sizeof(first));
    std::memcpy(&count, output.range.data() + 10, sizeof(count));
    if (reg < first || !count || reg - first >= count) return false;
    const uint64_t index = uint64_t(base) + reg - first;
    if (index >= MaxDescriptorIndex || !Read(host, value.descriptorArray, uintptr_t(index) * 8, output.descriptor) ||
        !output.descriptor) return false;
    failure = Failure::DescriptorMismatch;
    if (output.descriptor != expected) return false;
    // Successful Read already proves this exact map address cannot wrap.
    output.shaderRegister = reg; output.descriptorIndex = uint32_t(index);
    output.mapAddress = value.layout + mapOffset;
    output.rangeIndex = rangeIndex; output.rootParameter = output.range[14];
    return true;
}

template<class Host> bool ReadTexture(Host& host, uintptr_t registry, uint32_t handle,
                                      bool uav, TextureBinding& output)
{
    if (!handle || handle > TextureSlots) return false;
    const auto offset = uintptr_t(0x2f1d8) + uintptr_t(handle - 1) * 0xb0;
    if (!Read(host, registry, offset - 8, output.refs) || output.refs <= 0 ||
        !Read(host, registry, offset, output.native) || !output.native ||
        !Read(host, registry, offset + 0x48, output.requestedSrvState) ||
        !Read(host, registry, offset + 0x4e, output.compact)) return false;
    output.handle = handle; output.slot = registry + offset;
    if (!uav)
        return Read(host, registry, offset + 0x30, output.descriptor) && output.descriptor;
    // Exact simple path at154081..1540ac: raw mip nibble==1 requests ALL
    // subresources and uses the first authored UAV descriptor. Refuse the
    // calculated-mip/multi-mip paths; do not create or guess another view.
    return (output.compact[6] >> 4) == 1 &&
        Read(host, registry, offset + 0x40, output.extra) && output.extra &&
        Read(host, output.extra, 0x28, output.uavArray) && output.uavArray &&
        Read(host, output.uavArray, 0, output.descriptor) && output.descriptor;
}

template<class Host> bool ReadCurrent(Host& host, uintptr_t image, uintptr_t list4,
                                      const Scope& scope, const Receipt& receipt,
                                      const TextureHandles& handles, Snapshot& output, Failure& failure)
{
    uintptr_t currentCache = 0, descriptor = 0, currentList4 = 0;
    failure = Failure::CurrentScope;
    if (!CyberpunkRayConstants::Detail::Current(host, scope, currentCache, descriptor) ||
        currentCache != receipt.cache || descriptor != receipt.descriptor ||
        !Read(host, scope.engine, 0x40, currentList4) || !currentList4 || currentList4 != list4 ||
        !Read(host, currentCache, 0x68, output.layout) || !output.layout ||
        !Read(host, currentCache, 0x28, output.descriptorArray) || !output.descriptorArray ||
        !Read(host, image, 0x3438a28, output.registry) || !output.registry) return false;
    output.scope = scope; output.callerRva = DispatchReturnRva;
    output.list4 = list4; output.cache = currentCache;
    // Native1f3978 type2 map:453+stage*1c+2*register. UAV type1
    // map:8c3+stage*26+2*register. Neither is the ordinary SRV map.
    if (!ReadBinding(host, output, 0x453 + 2 * 0x1c + 2 * 6, 6, descriptor, output.b6, failure)) return false;
    const uint32_t selected[] = { handles.motion, handles.radiance, handles.hit };
    const uint32_t regs[] = { 4, 0, 8 };
    for (size_t i = 0; i < 3; ++i)
    {
        failure = Failure::TextureSource;
        if (!ReadTexture(host, output.registry, selected[i], i != 0, output.textures[i])) return false;
        const auto map = i ? 0x8c3 + 2 * 0x26 + 2 * regs[i] : 0x4c3 + 2 * 0x100 + 2 * regs[i];
        if (!ReadBinding(host, output, map, regs[i], output.textures[i].descriptor,
                         output.textures[i].binding, failure)) return false;
    }
    return true;
}
} // namespace Detail

// Metadata only: matching descriptor integers joins an original CPU upload to
// the selected actual cache slot, NOT immutable GPU CBV contents or producer
// completion. No COM ownership, descriptor heap lifetime, graph alias lease,
// shader-library identity or GPU state is established here. Host must invalidate
// the receipt on every later/nested upload and verify current list generation,
// frame/view/graph selection at the actual API boundary. List and List4 interface
// addresses need not equal; both are matched to the original engine context.
// Both complete reads must retain the same binding identities. Positive handle
// refcount changes are reported, not treated as an identity change or a lease.
template<class Host>
bool Observe(Host& host, uintptr_t image, uintptr_t caller, uintptr_t list4,
             const Scope& scope, const Receipt& receipt, const TextureHandles& handles,
             Snapshot& output, Failure* reason = nullptr, ChangedSnapshots* changed = nullptr) noexcept
{
    output = {};
    if (changed) *changed = {};
    Failure failure = Failure::Caller;
    bool success = false;
    try
    {
        if (image && caller >= image && caller - image == DispatchReturnRva && list4)
        {
            failure = Failure::UploadReceipt;
            if (receipt.phase == CyberpunkRayConstants::Phase::Uploaded && !receipt.source &&
                receipt.scope == scope && receipt.cache && receipt.descriptor &&
                (receipt.callerRva == CyberpunkRayConstants::UploadReturnRvas[0] ||
                 receipt.callerRva == CyberpunkRayConstants::UploadReturnRvas[1]))
            {
                Snapshot first, second;
                if (Detail::ReadCurrent(host, image, list4, scope, receipt, handles, first, failure) &&
                    Detail::ReadCurrent(host, image, list4, scope, receipt, handles, second, failure))
                {
                    failure = Failure::Changed;
                    if (first != second && changed) *changed = { first, second, true };
                    if (SameBindingIdentity(first, second))
                    { output = second; failure = Failure::None; success = true; }
                }
            }
        }
    }
    catch (...) { failure = Failure::ReadException; }
    if (reason) *reason = failure;
    return success;
}
} // namespace FSRD::CyberpunkRayBindings
